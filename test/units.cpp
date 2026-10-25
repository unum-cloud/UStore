/**
 *  @file test/units.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Unit tests of the blobs, documents and graph modalities through the C++ wrappers.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int32_t`, `std::uint32_t`, `std::uint64_t`
#include <cstdio>  // `stderr`
#include <cstdlib> // `std::free`
#include <cstring> // `std::memcpy`

#include <algorithm>   // `std::ranges::find`
#include <array>       // `std::array`
#include <atomic>      // `std::atomic`, `std::atomic_flag`, `std::atomic_ref`
#include <random>      // `std::mt19937_64`
#include <span>        // `std::span`
#include <string>      // `std::string`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`
#include <vector>      // `std::vector`

#include <fmt/format.h>
#include <yyjson.h>

#include <smashtable/basic_vector.hpp>

#include "ustore/ustore.hpp"

#include "ustore/utilities/ram_device.hpp" // `caller_exclusivity_t`, `executor_t`

#include "engine.hpp"      // `engine_t`, `handle_t`, `c_handle`, `view_t`
#include "harness.hpp"     // `suite_exit_code`, `expect`, `expect_eq`, `expect_status`, `spin_until`
#include "pages/model.hpp" // `model_page_t`, `model_edit_t`, `node_spec_t`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::utilities;
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

#pragma region Ranges

/** Checks that the postfix steps of a strided iterator move it and answer where it stood, as every iterator's do. */
std::size_t strided_iterator_postfix_steps() {
    std::size_t failures = 0;
    std::array<ustore_key_t, 2> const keys {key_from_integer(1), key_from_integer(2)};
    strided_iterator<ustore_key_t const> iterator = strided(keys).begin();
    failures += expect(*iterator++ == keys[0]);
    failures += expect(*iterator == keys[1]);
    failures += expect(*iterator-- == keys[1]);
    failures += expect(*iterator == keys[0]);
    return failures;
}

#pragma endregion Ranges

#pragma region Binary Modality

/** Checks that every key @p reference names holds a value of @p expected_length, or is missing. */
template <typename locations_type_>
std::size_t check_length(blobs_read_ref<locations_type_>& reference, ustore_length_t expected_length) {
    std::size_t failures = 0;
    bool const expects_missing = expected_length == missing_length_k;
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
std::size_t check_equalities(reference_type_<locations_type_>& reference, std::span<value_view_t const> values) {
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
        value_view_t const retrieved_view = *iterator;
        failures += expect_eq(retrieved_view.size(), values[index].size());
        failures += expect(retrieved_view == values[index]);
    }
    return failures;
}

/** Assigns @p values through @p reference and checks they read back as @p expected. */
template <typename locations_type_, typename values_type_>
std::size_t round_trip(blobs_ref<locations_type_>& reference, values_type_ const& values,
                       std::span<value_view_t const> expected) {
    std::size_t failures = expect(reference.assign(values).succeeded());
    failures += check_equalities(reference, expected);
    return failures;
}

/** Three one-byte values under three keys, exposed in each layout a batch of strings takes. */
struct triplet_t {
    static constexpr std::size_t value_size_k = sizeof(char);

    std::array<ustore_key_t, 3> keys {key_from_integer('a'), key_from_integer('b'), key_from_integer('c')};
    std::array<char, 3> values {'A', 'B', 'C'};
    std::array<std::uint32_t, 4> narrow_offsets {0, 1, 2, 3};
    std::array<std::uint64_t, 4> wide_offsets {0, 1, 2, 3};

    /** The values as a tape of 32-bit offsets. */
    ustore_strings_view_t u32_layout() const noexcept { return u32_tape(values.data(), narrow_offsets.data()); }

    /** The values as a tape of 64-bit offsets. */
    ustore_strings_view_t u64_layout() const noexcept { return u64_tape(values.data(), wide_offsets.data()); }

    /** The values one view each, which a write reads through an accessor. */
    std::array<value_view_t, 3> views() const noexcept {
        return {value_view_t {&values[0], 1}, value_view_t {&values[1], 1}, value_view_t {&values[2], 1}};
    }
};

/** Round-trips @p triplet through @p reference as a tape of either offset width and through an accessor. */
template <typename locations_type_>
std::size_t round_trip(blobs_ref<locations_type_>& reference, triplet_t const& triplet) {
    std::array<value_view_t, 3> const views = triplet.views();
    std::size_t failures = 0;
    failures += round_trip(reference, triplet.u32_layout(), views);
    failures += round_trip(reference, triplet.u64_layout(), views);
    failures += round_trip(reference, views, views);
    return failures;
}

/** Checks that @p reference holds @p triplet. */
template <template <typename> typename reference_type_, typename locations_type_>
std::size_t check_equalities(reference_type_<locations_type_>& reference, triplet_t const& triplet) {
    return check_equalities(reference, triplet.views());
}

/** Checks overwrites, cleared values keeping their keys, key streaming and removal on @p collection. */
std::size_t check_binary_collection(blobs_collection_t& collection) {
    std::size_t failures = 0;
    triplet_t triplet;
    auto reference = collection[triplet.keys];
    failures += round_trip(reference, triplet);

    // Overwrite with values of the same size
    for (char& value : triplet.values) value += 7;
    failures += round_trip(reference, triplet);

    // Empty values keep their keys
    failures += expect(reference.clear_values().succeeded());
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

    failures += expect(reference.remove().succeeded());
    failures += check_length(reference, missing_length_k);
    return failures;
}

/** Opens and clears a database, then round-trips values through the main collection. */
std::size_t database_open_clear_close(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    blobs_collection_t collection = database.main();
    failures += check_binary_collection(collection);
    return failures;
}

/** Clearing the whole database empties the main collection. */
std::size_t database_clear_collection_by_clearing_database(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    blobs_collection_t collection = database.main();
    triplet_t triplet;
    auto reference = collection[triplet.keys];
    failures += round_trip(reference, triplet);

    failures += expect(collection.keys().size() == 3u);
    failures += expect(collection.pairs().size() == 3u);

    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    failures += check_length(reference, missing_length_k);
    return failures;
}

/** A description names the engine the suite opened, whether the head describes itself blocking or deferred. */
std::size_t database_describe(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(open_status.succeeded() && executor_status.succeeded());
    if (failures != 0) return failures;

    auto [description, describe_status] = database.describe();
    auto [deferred_description, deferred_status] = database.deferred(executor).describe().wait();
    failures += expect(describe_status.succeeded() && deferred_status.succeeded());
    failures += expect_eq(std::string_view {ustore_engine_name(description.engine)}, config.engine());
    failures += expect_eq(std::string_view {ustore_engine_name(deferred_description.engine)}, config.engine());
    return failures;
}

/** Clearing the pairs keeps a named collection while emptying it, and a deferred clear of the collections drops it. */
case_result_t database_clear_modes(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(open_status.succeeded() && executor_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_collections())
        return case_result_t::skipped("the engine keeps no named collections", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    auto [kept, create_status] = database.create("kept");
    failures += expect(create_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(kept[key_from_integer(5)].assign("value").succeeded());

    failures += expect(database.clear(ustore_drop_pairs_k).succeeded());
    failures += expect(*database.contains("kept"));
    failures += expect(kept.keys().size() == 0u);

    failures += expect(database.deferred(executor).clear(ustore_drop_collection_k).wait().succeeded());
    failures += expect(!*database.contains("kept"));
    return failures;
}

/** A compaction its progress callable cancels at the first report answers cancelled, blocking and deferred alike, and
 *  the data still reads. */
std::size_t database_compact(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(open_status.succeeded() && executor_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    blobs_collection_t collection = database.main();
    failures += expect(collection[key_from_integer(11)].assign("compacted").succeeded());

    std::size_t reports = 0;
    auto cancelling = [&](ustore_size_t, ustore_size_t) noexcept {
        ++reports;
        return ustore_cancel_k;
    };
    failures += expect_status(database.compact(cancelling), status_code_t::cancelled_k);
    failures += expect_status(database.deferred(executor).compact(cancelling).wait(), status_code_t::cancelled_k);
    failures += expect_eq(reports, 2u);
    failures += expect(database.compact().succeeded());
    failures += expect(collection[key_from_integer(11)].value() == "compacted");
    return failures;
}

/** Fills keys 1000 to 1100 and 900 down to 800, then overwrites every other key across both ranges. */
std::size_t database_overwrite_with_step(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    blobs_collection_t collection = database.main();

    for (std::uint64_t key = 1000; key != 1100; ++key)
        failures += expect(collection[key_from_integer(key)].assign("some").succeeded());
    for (std::uint64_t key = 1000; key != 1100; ++key)
        failures += expect(*collection[key_from_integer(key)].value() == "some");
    failures += expect(collection.keys().size() == 100u);
    failures += expect(collection.pairs().size() == 100u);

    for (std::uint64_t key = 900; key != 800; --key)
        failures += expect(collection[key_from_integer(key)].assign("other").succeeded());
    for (std::uint64_t key = 900; key != 800; --key)
        failures += expect(*collection[key_from_integer(key)].value() == "other");
    failures += expect(collection.keys().size() == 200u);
    failures += expect(collection.pairs().size() == 200u);

    for (std::uint64_t key = 800; key != 1100; key += 2)
        failures += expect(collection[key_from_integer(key)].assign("third").succeeded());
    for (std::uint64_t key = 800; key != 1100; key += 2)
        failures += expect(*collection[key_from_integer(key)].value() == "third");
    failures += expect(collection.keys().size() == 250u);
    failures += expect(collection.pairs().size() == 250u);
    return failures;
}

/** Populates the database, closes and reopens it, and checks the contents survived. */
std::size_t database_persistency(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    triplet_t triplet;
    {
        blobs_collection_t main_collection = database.main();
        auto main_reference = main_collection[triplet.keys];
        failures += check_length(main_reference, missing_length_k);
        failures += round_trip(main_reference, triplet);
        failures += check_length(main_reference, triplet_t::value_size_k);

        if (database.supports_collections()) {
            blobs_collection_t named_collection = *database.create("collection");
            auto named_reference = named_collection[triplet.keys];
            failures += check_length(named_reference, missing_length_k);
            failures += round_trip(named_reference, triplet);
            failures += check_length(named_reference, triplet_t::value_size_k);
            failures += expect(named_collection.clear_values().succeeded());
            failures += check_length(named_reference, 0);
        }
    }
    database.close();
    {
        auto [reopened, reopen_status] = open_database<database_t>(config);
        failures += expect(reopen_status.succeeded());
        if (failures != 0) return failures;
        database = std::move(reopened);

        blobs_collection_t main_collection = database.main();
        auto main_reference = main_collection[triplet.keys];
        failures += check_equalities(main_reference, triplet);
        failures += check_length(main_reference, triplet_t::value_size_k);
        failures += expect(main_collection.keys().size() == 3u);
        failures += expect(main_collection.pairs().size() == 3u);

        if (database.supports_collections()) {
            failures += expect(database.contains("collection"));
            failures += expect(*database.contains("collection"));
            blobs_collection_t named_collection = *database["collection"];
            auto named_reference = named_collection[triplet.keys];
            failures += check_length(named_reference, 0);
            failures += expect(named_collection.keys().size() == 3u);
            failures += expect(named_collection.pairs().size() == 3u);
        }
    }
    return failures;
}

/** Creates, looks up, drops and clears collections under unique names. */
std::size_t database_named_collections(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

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

    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    failures += expect(*database.contains(""));
    return failures;
}

/** Threads racing to create one name leave exactly one collection behind, which the name addresses. */
case_result_t database_named_collections_race(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_collections())
        return case_result_t::skipped("the engine keeps no named collections", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    constexpr std::size_t threads_count = 8;
    std::atomic<std::size_t> created {0};
    std::atomic<std::size_t> refused {0};
    auto task = [&](std::size_t) noexcept {
        auto [collection, create_status] = database.create("contested");
        if (create_status.succeeded()) created.fetch_add(1, std::memory_order_relaxed);
        else if (create_status == status_code_t::already_exists_k) refused.fetch_add(1, std::memory_order_relaxed);
    };
    auto maybe_executor = executor_t::make(threads_count, caller_exclusivity_t::caller_inclusive_k);
    failures += expect(maybe_executor);
    if (!maybe_executor) return failures;
    failures += expect(st::succeeded(maybe_executor->for_threads(task)));

    failures += expect_eq(created.load(), 1u);
    failures += expect_eq(refused.load(), threads_count - 1);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    auto [collections, list_status] = database.collections(arena);
    auto [found, find_status] = database.find("contested");
    failures += expect(list_status.succeeded() && find_status.succeeded());
    failures += expect_eq(collections.size(), 1u);
    failures += expect(collections.size() == 1 && ustore_collection_id_t(found) == collections[0]);
    failures += expect(database.drop("contested").succeeded());
    return failures;
}

/** Lists every collection with its name and modality beside its identifier, an unnamed one with an empty name, where
 *  each kind of handle creates its own modality and finds no other. */
case_result_t database_named_collections_list(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_collections())
        return case_result_t::skipped("the engine keeps no named collections", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    // Every kind of collection handle creates a collection of its own modality
    std::array<std::string_view, 3> const names {"col1", "col2", "col3"};
    std::array<ustore_modality_t, 3> const modalities {ustore_modality_blobs_k, ustore_modality_docs_k,
                                                       ustore_modality_graph_k};
    failures += expect(database.create<blobs_collection_t>(names[0]).has_value());
    failures += expect(database.create<docs_collection_t>(names[1]).has_value());
    failures += expect(database.create<graph_collection_t>(names[2]).has_value());
    ustore_outcome_t outcome {};
    ustore_collection_id_t unnamed = ustore_collection_main_k;
    ustore_collection_create_t create {};
    create.database = database;
    create.outcome = &outcome;
    create.modality = ustore_modality_strings_k;
    create.collection = &unnamed;
    ustore_collection_create(&create);
    failures += expect_success(outcome);

    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    ustore_size_t written = 0;
    ustore_collection_id_t const* ids = nullptr;
    ustore_modality_t const* listed_modalities = nullptr;
    ustore_strings_view_t listed_names {};
    ustore_collection_list_t list {};
    list.database = database;
    list.outcome = &outcome;
    list.collections = &ids;
    list.modalities = &listed_modalities;
    list.names = &listed_names;
    list.written = &written;
    list.arena = arena;
    ustore_collection_list(&list);
    failures += expect_success(outcome);
    failures += expect_eq(written, 4u);
    if (written != 4) return failures;

    // Every listed name resolves back to the identifier beside it, and the unnamed collection lists an empty name
    for (std::size_t index = 0; index != written; ++index) {
        std::string_view const name = string_of(tape_string(listed_names, index));
        if (name.empty()) {
            failures += expect(ids[index] == unnamed && listed_modalities[index] == ustore_modality_strings_k);
            continue;
        }
        auto const named = std::ranges::find(names, name);
        failures += expect(named != names.end());
        if (named == names.end()) continue;
        failures += expect(listed_modalities[index] == modalities[named - names.begin()]);
        auto maybe_found = collection_named(database.receiver(), name);
        failures += expect(maybe_found && *maybe_found == ids[index]);
    }

    // A typed lookup refuses a collection of another modality, and a typed creation makes one the graph calls serve
    failures += expect(database.find<docs_collection_t>("col3") == status_code_t::invalid_argument_k);
    failures += expect(database.find<graph_collection_t>("col3").has_value());
    auto friends = database.find_or_create<graph_collection_t>("friends");
    failures += expect(friends && friends->write_edge(edge_t {key_from_integer(1), key_from_integer(2)}).succeeded());

    failures += expect(database.drop("col1").succeeded());
    failures += expect(!*database.contains("col1"));
    failures += expect(database.drop("").failed());
    failures += expect(database.main().clear().succeeded());
    return failures;
}

/** Every call resolves its collections, refusing identifiers nobody created and collections keyed the other way. */
case_result_t database_collections_resolve(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_collections()) return case_result_t::skipped("the engine keeps no collections", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    // Nothing reaches an identifier the engine never handed out, whatever it may keep there itself
    for (ustore_collection_id_t const unknown :
         {ustore_collection_id_t {uuid_from_integer(4242)}, ustore_collection_id_t {largest_key_k.uuid}}) {
        blobs_collection_t keyed {database.receiver(), unknown};
        failures += expect(keyed[key_from_integer(1)].value() == status_code_t::not_found_k);
        failures += expect(keyed[key_from_integer(1)].assign("intruder") == status_code_t::not_found_k);
        failures += expect(keyed.clear() == status_code_t::not_found_k);
        strings_collection_t stringed {database.receiver(), unknown};
        failures += expect(stringed.assign("intruder", key_from_integer(1)) == status_code_t::not_found_k);
    }

    // A modality fixes how a collection is keyed, and calls keyed the other way are refused
    auto maybe_words = database.create<strings_collection_t>("words");
    auto maybe_numbers = database.create("numbers");
    failures += expect(maybe_words && maybe_numbers);
    if (!maybe_words || !maybe_numbers) return failures;
    ustore_collection_id_t const words = *maybe_words;
    blobs_collection_t keyed_words {database.receiver(), words};
    failures += expect(keyed_words[key_from_integer(1)].assign("number") == status_code_t::invalid_argument_k);
    strings_collection_t stringed_numbers {database.receiver(), *maybe_numbers};
    failures += expect(stringed_numbers.assign("word", key_from_integer(1)) == status_code_t::invalid_argument_k);
    strings_collection_t stringed_main {database.receiver(), ustore_collection_main_k};
    failures += expect(stringed_main.assign("word", key_from_integer(1)) == status_code_t::invalid_argument_k);
    failures += expect(maybe_words->assign("word", key_from_integer(1)).succeeded());

    // A name is never a collection, and dropping a collection by identifier takes its name too
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    auto [listed, list_status] = database.collections(arena);
    failures += expect(list_status.succeeded());
    failures += expect_eq(listed.size(), 2u);
    ustore_outcome_t outcome {};
    ustore_collection_drop_t drop {};
    drop.database = database;
    drop.outcome = &outcome;
    drop.collection = words;
    drop.mode = ustore_drop_collection_k;
    ustore_collection_drop(&drop);
    failures += expect_success(outcome);
    failures += expect(!*database.contains("words"));
    failures += expect(database.drop("numbers").succeeded());
    return failures;
}

/** Clearing values keeps the keys and empties their binary strings. */
std::size_t database_clear_values(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    triplet_t triplet;
    blobs_collection_t collection = database.main();
    auto reference = collection[triplet.keys];

    failures += check_length(reference, missing_length_k);
    failures += round_trip(reference, triplet);
    failures += check_length(reference, triplet_t::value_size_k);

    failures += expect(collection.clear_values().succeeded());
    failures += check_length(reference, 0);
    failures += expect(collection.clear().succeeded());
    failures += check_length(reference, missing_length_k);
    return failures;
}

/** Reads presences through the C ABI and the C++ wrappers. */
std::size_t database_presences(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    blobs_collection_t main_collection = database.main();

    constexpr std::size_t keys_count = 1000;
    for (std::size_t index = 0; index != keys_count; ++index)
        if (index % 10) failures += expect(main_collection[key_from_integer(index)].assign("value").succeeded());

    std::array<ustore_key_t, keys_count> keys;
    for (std::size_t index = 0; index != keys_count; ++index) keys[index] = key_from_integer(index);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    ustore_octet_t const* found_presences = nullptr;
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read {};
    read.database = database;
    read.outcome = &outcome;
    read.count = keys_count;
    read.keys = {keys.data(), sizeof(ustore_key_t)};
    read.presences = &found_presences;
    read.arena = arena;

    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(found_presences != nullptr);
    for (std::size_t index = 0; found_presences && index != keys_count; ++index)
        failures += expect(bits_view_t {found_presences}[index] == (index % 10 != 0));

    auto maybe_presences = main_collection[keys].present();
    failures += expect(maybe_presences);
    auto const& presences = *maybe_presences;
    for (std::size_t index = 0; index != keys_count; ++index)
        failures += expect(static_cast<bool>(presences[index]) == (index % 10 != 0));
    return failures;
}

/** One arena serves the reads of two collections in turn, each read rewinding it, while shorthand reads keep no arena
 *  anywhere and own the one-off arena they answer from. */
case_result_t database_arena_reuse(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_collections())
        return case_result_t::skipped("the engine keeps no named collections", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    auto [first, first_status] = database.create("first");
    auto [second, second_status] = database.create("second");
    failures += expect(first_status.succeeded() && second_status.succeeded());
    if (failures != 0) return failures;
    std::array<ustore_key_t, 2> const keys {key_from_integer(1), key_from_integer(2)};
    std::array<value_view_t, 2> const first_values {"alpha", "beta"};
    std::array<value_view_t, 2> const second_values {"gamma", "a much longer delta"};
    failures += expect(first[keys].assign(first_values).succeeded());
    failures += expect(second[keys].assign(second_values).succeeded());

    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    for (std::size_t round = 0; round != 2; ++round) {
        auto [from_first, first_read_status] = first.read(keys, arena);
        failures += expect(first_read_status.succeeded());
        for (std::size_t index = 0; first_read_status.succeeded() && index != keys.size(); ++index)
            failures += expect(from_first[index] == first_values[index]);

        auto [from_second, second_read_status] = second.read(keys, arena);
        failures += expect(second_read_status.succeeded());
        for (std::size_t index = 0; second_read_status.succeeded() && index != keys.size(); ++index)
            failures += expect(from_second[index] == second_values[index]);
    }

    // The shorthand answers outlive the collection handles that read them, since each owns its own arena
    expected<owned_value_t> kept = first[keys[0]].value();
    expected<owned<values_view_t>> kept_batch = second[keys].value();
    first = blobs_collection_t {};
    second = blobs_collection_t {};
    failures += expect(kept == first_values[0]);
    failures += expect(kept_batch && kept_batch->size() == 2 && (*kept_batch)[1] == second_values[1]);
    failures += expect(database.drop("first").succeeded() && database.drop("second").succeeded());
    return failures;
}

/** A key stream visits every written key in order. */
std::size_t database_scan(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    blobs_collection_t collection = database.main();

    constexpr std::size_t keys_count = 1000;
    std::array<ustore_key_t, keys_count> keys;
    for (std::size_t index = 0; index != keys_count; ++index) keys[index] = key_from_integer(index);
    auto reference = collection[keys];
    value_view_t const value("value");
    failures += expect(reference.assign(value).succeeded());
    auto [stream, stream_status] = keys_stream_t::make(database.receiver(), collection, smallest_key_k, 256);
    failures += expect(stream_status.succeeded());
    std::size_t count = 0;
    while (!stream.is_end()) {
        failures += expect_eq(stream.key(), key_from_integer(count++));
        ++stream;
    }
    failures += expect_eq(count, keys_count);
    return failures;
}

/** Ordered batched scan over the main collection. */
std::size_t database_batch_scan(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    blobs_collection_t collection = database.main();

    std::array<ustore_key_t, 512> keys;
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = key_from_integer(index);
    auto reference = collection[keys];
    value_view_t const value("value");
    failures += expect(reference.assign(value).succeeded());
    auto [stream, stream_status] = keys_stream_t::make(database.receiver(), collection, smallest_key_k, 256);
    failures += expect(stream_status.succeeded());
    auto batch = stream.keys_batch();
    failures += expect_eq(batch.size(), 256u);
    failures += expect(!stream.is_end());
    for (std::size_t index = 0; index != 256; ++index) failures += expect_eq(batch[index], key_from_integer(index));

    failures += expect(stream.seek_to_next_batch().succeeded());
    batch = stream.keys_batch();
    failures += expect_eq(batch.size(), 256u);
    failures += expect(!stream.is_end());
    for (std::size_t index = 0; index != 256; ++index)
        failures += expect_eq(batch[index], key_from_integer(index + 256));

    failures += expect(stream.seek_to_next_batch().succeeded());
    batch = stream.keys_batch();
    failures += expect_eq(batch.size(), 0u);
    failures += expect(stream.is_end());
    return failures;
}

/** Readers cannot see the writes of an uncommitted transaction, https://jepsen.io/consistency/models/read-committed */
case_result_t database_transaction_read_committed(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_transactions()) return case_result_t::skipped("the engine keeps no transactions", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    failures += expect(database.transaction());
    transaction_t transaction = *database.transaction();

    triplet_t triplet;
    auto transaction_reference = transaction[triplet.keys];
    failures += round_trip(transaction_reference, triplet);

    blobs_collection_t collection = database.main();
    auto collection_reference = collection[triplet.keys];

    failures += check_length(collection_reference, missing_length_k);
    failures += expect(transaction.commit().has_value());
    failures += expect(transaction.reset().succeeded());

    failures += check_equalities(collection_reference, triplet);
    return failures;
}

/** Reads through a snapshot miss writes made after it, https://jepsen.io/consistency/models/snapshot-isolation */
case_result_t database_transaction_snapshot_isolation(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_snapshots()) return case_result_t::skipped("the engine keeps no snapshots", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    triplet_t triplet;
    triplet_t same_values_triplet;
    same_values_triplet.values = {'D', 'D', 'D'};

    blobs_collection_t collection = database.main();
    auto collection_reference = collection[triplet.keys];

    failures += check_length(collection_reference, missing_length_k);
    failures += round_trip(collection_reference, triplet);

    auto snapshot = *database.snapshot();
    auto snapshot_reference = snapshot[triplet.keys];
    failures += check_equalities(snapshot_reference, triplet);
    failures += round_trip(collection_reference, same_values_triplet);

    auto maybe_retrieved = snapshot_reference.value();
    failures += expect(maybe_retrieved);
    if (!maybe_retrieved) return failures;
    auto const& retrieved = *maybe_retrieved;
    auto iterator = retrieved.begin();
    for (value_view_t const newer_view : same_values_triplet.views()) {
        value_view_t const retrieved_view = *iterator;
        failures += expect_eq(retrieved_view.size(), newer_view.size());
        failures += expect(retrieved_view != newer_view);
        ++iterator;
    }

    snapshot = *database.snapshot();
    auto reference = snapshot[same_values_triplet.keys];
    failures += check_equalities(reference, same_values_triplet);
    return failures;
}

/** A transaction reads the head while a snapshot reads its own view. */
case_result_t database_transaction_with_snapshot(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_snapshots()) return case_result_t::skipped("the engine keeps no snapshots", failures);
    if (!database.supports_transactions()) return case_result_t::skipped("the engine keeps no transactions", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    triplet_t triplet;
    triplet_t same_values_triplet;
    same_values_triplet.values = {'D', 'D', 'D'};

    blobs_collection_t collection = database.main();
    auto collection_reference = collection[triplet.keys];

    failures += check_length(collection_reference, missing_length_k);
    failures += round_trip(collection_reference, triplet);

    auto snapshot = *database.snapshot();
    auto snapshot_reference = snapshot[triplet.keys];
    failures += check_equalities(snapshot_reference, triplet);

    failures += round_trip(collection_reference, same_values_triplet);

    transaction_t transaction = *database.transaction();
    auto head_reference = transaction[triplet.keys];
    failures += check_equalities(head_reference, same_values_triplet);
    auto older_reference = snapshot[triplet.keys];
    failures += check_equalities(older_reference, triplet);

    snapshot = *database.snapshot();
    auto newer_reference = snapshot[triplet.keys];
    failures += check_equalities(newer_reference, same_values_triplet);
    return failures;
}

/** Saves snapshots as databases, then opens and modifies them independently of the original. */
case_result_t database_save_snapshot(config_t const& config) {
    std::size_t failures = 0;
    path_t const original_directory {config.directory(), "original/"};
    path_t const first_saved_directory {config.directory(), "saved1/"};
    path_t const second_saved_directory {config.directory(), "saved2/"};
    failures += expect(recreate_directory(original_directory.c_str()));
    failures += expect(recreate_directory(first_saved_directory.c_str()));
    failures += expect(recreate_directory(second_saved_directory.c_str()));

    config_t const original_config {config.options(), original_directory.c_str()};
    auto [database, open_status] = open_database<database_t>(original_config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_snapshots()) return case_result_t::skipped("the engine keeps no snapshots", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    triplet_t triplet;
    triplet_t same_values_triplet;
    same_values_triplet.values = {'D', 'D', 'D'};

    blobs_collection_t collection = database.main();
    auto collection_reference = collection[triplet.keys];
    failures += round_trip(collection_reference, triplet);

    auto first_snapshot = *database.snapshot();
    failures += expect(first_snapshot.save(first_saved_directory.c_str()).succeeded());
    failures += check_equalities(collection_reference, triplet);

    config_t const first_config {config.options(), first_saved_directory.c_str()};
    auto [first_saved, first_saved_status] = open_database<database_t>(first_config);
    failures += expect(first_saved_status.succeeded());
    if (failures != 0) return failures;
    auto first_collection = first_saved.main();
    auto first_reference = first_collection[triplet.keys];
    failures += check_equalities(first_reference, triplet);

    failures += round_trip(collection_reference, same_values_triplet);

    auto second_snapshot = *database.snapshot();
    failures += expect(second_snapshot.save(second_saved_directory.c_str()).succeeded());
    failures += check_equalities(collection_reference, same_values_triplet);

    config_t const second_config {config.options(), second_saved_directory.c_str()};
    auto [second_saved, second_saved_status] = open_database<database_t>(second_config);
    failures += expect(second_saved_status.succeeded());
    if (failures != 0) return failures;
    auto second_collection = second_saved.main();
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

/** A save hands its progress callable the bytes saved and the bytes in total, never decreasing, never past the total
 *  and ending at it, whether the snapshot saves itself blocking or deferred. */
case_result_t database_save_progress(config_t const& config) {
    std::size_t failures = 0;
    path_t const blocking_directory {config.directory(), "saved.blocking/"};
    path_t const deferred_directory {config.directory(), "saved.deferred/"};
    failures += expect(recreate_directory(blocking_directory.c_str()));
    failures += expect(recreate_directory(deferred_directory.c_str()));
    auto [database, open_status] = open_database<database_t>(config);
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(open_status.succeeded() && executor_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_snapshots()) return case_result_t::skipped("the engine keeps no snapshots", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    triplet_t triplet;
    blobs_collection_t collection = database.main();
    auto reference = collection[triplet.keys];
    failures += round_trip(reference, triplet);

    // Engines reporting per collection then report before the end, where the order of the counts shows
    if (database.supports_collections()) {
        auto [named, create_status] = database.create("saved");
        failures += expect(create_status.succeeded());
        if (failures != 0) return failures;
        auto named_reference = named[triplet.keys];
        failures += round_trip(named_reference, triplet);
    }

    std::size_t misordered = 0;
    ustore_size_t last_finished = 0;
    ustore_size_t last_total = 0;
    auto recording = [&](ustore_size_t finished, ustore_size_t total) noexcept {
        misordered += finished < last_finished || finished > total;
        last_finished = finished;
        last_total = total;
        return ustore_continue_k;
    };
    auto snapshot = *database.snapshot();
    failures += expect(snapshot.save(blocking_directory.c_str(), recording).succeeded());
    failures += expect(last_total != 0 && last_finished == last_total);

    last_finished = 0;
    last_total = 0;
    failures += expect(snapshot.deferred(executor).save(deferred_directory.c_str(), recording).wait().succeeded());
    failures += expect(last_total != 0 && last_finished == last_total);
    failures += expect_eq(misordered, 0u);
    return failures;
}

/** One thread reads through a snapshot while another replaces it with a newer one. */
case_result_t database_snapshot_with_threads(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_snapshots()) return case_result_t::skipped("the engine keeps no snapshots", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

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
            snapshot = *database.snapshot();
            is_reset = true;
            snapshot_lock.clear(std::memory_order_release);
        }
        else { ++local_failures; }
        thread_failures.fetch_add(local_failures, std::memory_order_relaxed);
    };

    auto maybe_executor = executor_t::make(2, caller_exclusivity_t::caller_inclusive_k);
    failures += expect(maybe_executor);
    if (!maybe_executor) return failures;
    failures += expect(st::succeeded(maybe_executor->for_threads(task)));
    failures += thread_failures.load();
    return failures;
}

/** Concurrent transactions erasing missing keys both commit and leave the database empty. */
case_result_t database_transaction_erase_missing(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_transactions()) return case_result_t::skipped("the engine keeps no transactions", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    failures += expect(database.transaction());
    transaction_t first_transaction = *database.transaction();
    transaction_t second_transaction = *database.transaction();

    failures += expect(second_transaction.main().at(key_from_integer(7297309151944849401)).remove().succeeded());
    failures += expect(first_transaction.main().at(key_from_integer(8640850744835793378)).remove().succeeded());
    failures += expect(first_transaction.commit().has_value());
    failures += expect(second_transaction.commit().has_value());

    failures += expect(database.main().at(key_from_integer(8640850744835793378)).value() == value_view_t {});
    failures += expect(database.main().at(key_from_integer(7297309151944849401)).value() == value_view_t {});
    failures += expect(database.main().keys().size() == 0u);
    return failures;
}

/** Of two transactions writing the same key, the later commit fails. */
case_result_t database_transaction_write_conflicting(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_transactions()) return case_result_t::skipped("the engine keeps no transactions", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    failures += expect(database.transaction());
    transaction_t first_transaction = *database.transaction();
    transaction_t second_transaction = *database.transaction();

    failures += expect(second_transaction.main().at(key_from_integer(6)).assign("a").succeeded());
    failures += expect(first_transaction.main().at(key_from_integer(6)).assign("b").succeeded());
    failures += expect(first_transaction.commit().has_value());
    failures += expect(second_transaction.commit().failed());
    return failures;
}

/** A stage answers the stamp the transaction reads at, its commit a position no lower, and a later snapshot reads at
 *  or above that position. */
case_result_t database_transaction_stamps(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_transactions()) return case_result_t::skipped("the engine keeps no transactions", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    failures += expect(database.transaction());
    transaction_t transaction = *database.transaction();

    triplet_t triplet;
    auto transaction_reference = transaction[triplet.keys];

    failures += expect(transaction_reference.assign(triplet.u64_layout()).succeeded());
    auto [read_stamp, stage_status] = transaction.stage();
    failures += expect(stage_status.succeeded());
    auto [commit_stamp, commit_status] = transaction.commit();
    failures += expect(commit_status.succeeded());
    failures += expect(static_cast<ustore_stamp_t>(commit_stamp) > 0);
    failures += expect(static_cast<ustore_stamp_t>(read_stamp) <= static_cast<ustore_stamp_t>(commit_stamp));
    failures += expect(transaction.reset().succeeded());

    if (!database.supports_snapshots()) return failures;
    auto [snapshot, snapshot_status] = database.snapshot();
    failures += expect(snapshot_status.succeeded());
    failures += expect(static_cast<ustore_stamp_t>(commit_stamp) <= static_cast<ustore_stamp_t>(snapshot.read_stamp()));
    return failures;
}

/** A database destroyed before its transaction and snapshot stays open for both, each holding it until it goes, so the
 *  transaction still writes, commits and resets, and the snapshot still reads what it pinned. */
case_result_t database_freed_first(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_transactions() && !database.supports_snapshots())
        return case_result_t::skipped("the engine keeps neither transactions nor snapshots", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    ustore_key_t const key = key_from_integer(91);
    blobs_collection_t head = database.main();
    failures += expect(head[key].assign("before").succeeded());

    expected<transaction_t> transaction = status_code_t::unsupported_k;
    expected<snapshot_t> snapshot = status_code_t::unsupported_k;
    if (database.supports_transactions()) {
        transaction = database.transaction();
        failures += expect(transaction.has_value());
    }
    if (database.supports_snapshots()) {
        snapshot = database.snapshot();
        failures += expect(snapshot.has_value());
    }
    { database_t const freed = std::move(database); }

    if (transaction) {
        blobs_collection_t inside = transaction->main();
        failures += expect(inside[key].assign("after").succeeded());
        failures += expect(transaction->commit().has_value());
        failures += expect(transaction->reset().succeeded());
        failures += expect(inside[key].value() == "after");
    }
    if (snapshot) {
        blobs_read_collection_t pinned = snapshot->main();
        failures += expect(pinned[key].value() == "before");
    }
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

/** Three JSON texts laid end to end, with the offsets a tape reads them through. */
struct documents_tape_t {

    /** The three JSON texts laid end to end, with no separators. */
    char bytes[256] {};

    /** Where each text starts, and where the last one ends. */
    std::array<std::uint64_t, 4> offsets {};

    documents_tape_t() noexcept = default;
    documents_tape_t(documents_tape_t const&) = delete;
    documents_tape_t& operator=(documents_tape_t const&) = delete;

    /** Lays out @p documents, which together must fit in @c bytes. */
    void assign(std::array<std::string_view, 3> const& documents) noexcept {
        std::uint64_t offset = 0;
        for (std::size_t index = 0; index != documents.size(); ++index) {
            offsets[index] = offset;
            std::memcpy(bytes + offset, documents[index].data(), documents[index].size());
            offset += documents[index].size();
        }
        offsets[3] = offset;
    }

    /** A tape that follows every later @c assign. */
    ustore_strings_view_t values() const noexcept { return u64_tape(bytes, offsets.data()); }

    /** The texts as laid out now, one view each. */
    std::array<value_view_t, 3> views() const noexcept {
        std::array<value_view_t, 3> texts;
        for (std::size_t index = 0; index != texts.size(); ++index)
            texts[index] = value_view_t {bytes + offsets[index], offsets[index + 1] - offsets[index]};
        return texts;
    }
};

/** Maps integers to flat JSON documents and reads them whole, by field and as a binary string. */
std::size_t database_docs_flat(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    auto [collection, collection_status] = database.create<docs_collection_t>("docs");
    failures += expect(collection_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(collection[key_from_integer(1)].assign(flat_documents_k[0]).succeeded());
    failures += expect(collection[key_from_integer(2)].assign(flat_documents_k[1]).succeeded());
    failures += expect(collection[key_from_integer(3)].assign(flat_documents_k[2]).succeeded());
    failures += expect(json_equal(as_text(*collection[key_from_integer(1)].value()), flat_documents_k[0]));
    failures += expect(
        json_equal(as_text(*collection[collection_key_field_t {key_from_integer(2), "person"}].value()), R"("Bob")"));
    failures += expect(
        json_equal(as_text(*collection[collection_key_field_t {key_from_integer(3), "age"}].value()), "26"));

    auto maybe_person = collection[collection_key_field_t {key_from_integer(1), "person"}].value(
        ustore_docs_field_type_str_k);
    failures += expect_eq(std::string_view(maybe_person->c_str(), maybe_person->size()), std::string_view("Alice"));
    return failures;
}

/** Writes nested documents in one batch and reads them back by key and by JSON pointer. */
std::size_t database_docs_nested_batch(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    auto [collection, collection_status] = database.create<docs_collection_t>("docs");
    failures += expect(collection_status.succeeded());
    if (failures != 0) return failures;

    documents_tape_t tape;
    ustore_strings_view_t const values = tape.values();
    tape.assign(nested_documents_k);

    std::array<ustore_key_t, 3> keys {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
    auto reference = collection[keys];
    failures += expect(reference.assign(values).succeeded());

    failures += expect(json_equal(as_text(*collection[key_from_integer(1)].value()), nested_documents_k[0]));
    failures += expect(json_equal(as_text(*collection[key_from_integer(2)].value()), nested_documents_k[1]));
    failures += expect(json_equal(as_text(*collection[key_from_integer(3)].value()), nested_documents_k[2]));

    failures += expect(json_equal(as_text(*collection[collection_key_field_t {key_from_integer(1), "person"}].value()),
                                  R"({"name":"Alice","age":24})"));
    failures += expect(json_equal(as_text(*collection[collection_key_field_t {key_from_integer(2), "person"}].value()),
                                  R"([{"name":"Bob","age":25}])"));
    failures += expect(json_equal(
        as_text(*collection[collection_key_field_t {key_from_integer(2), "/person/0/name"}].value()), R"("Bob")"));

    failures += check_equalities(reference, tape.views());

    std::array<ustore_key_t, 3> unsorted_keys {key_from_integer(1), key_from_integer(3), key_from_integer(2)};
    auto unsorted_reference = collection[unsorted_keys];
    tape.assign({nested_documents_k[0], nested_documents_k[2], nested_documents_k[1]});
    failures += check_equalities(unsorted_reference, tape.views());

    std::array<ustore_key_t, 3> duplicate_keys {key_from_integer(1), key_from_integer(2), key_from_integer(1)};
    auto duplicate_reference = collection[duplicate_keys];
    tape.assign({nested_documents_k[0], nested_documents_k[1], nested_documents_k[0]});
    failures += check_equalities(duplicate_reference, tape.views());

    std::array<collection_key_field_t, 3> keys_with_fields {
        collection_key_field_t {key_from_integer(1), "person"},
        collection_key_field_t {key_from_integer(2), "/person/0/name"},
        collection_key_field_t {key_from_integer(3), "age"},
    };
    auto fields_reference = collection[keys_with_fields];
    tape.assign({R"({"age":24,"name":"Alice"})", R"("Bob")", "26"});
    failures += check_equalities(fields_reference, tape.views());

    tape.assign({nested_documents_k[0], nested_documents_k[1], R"({"name":"Alice", } "age": 24})"});
    failures += expect(reference.assign(values).failed());
    return failures;
}

/** Updates, inserts and upserts whole documents and fields addressed by JSON pointers. */
std::size_t database_docs_modify(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    auto [collection, collection_status] = database.create<docs_collection_t>("docs");
    failures += expect(collection_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(collection[key_from_integer(1)].assign(nested_documents_k[0].data()).succeeded());
    failures += expect(json_equal(as_text(*collection[key_from_integer(1)].value()), nested_documents_k[0]));

    char const* modifier = R"({"person":{"name":"Charles","age":28}})";
    failures += expect(collection[key_from_integer(1)].update(modifier).succeeded());
    failures += expect(json_equal(as_text(*collection[key_from_integer(1)].value()), modifier));

    failures += expect(collection[collection_key_field_t {key_from_integer(1), "/person"}]
                           .update(R"({"name":"Alice","age":24})")
                           .succeeded());
    failures += expect(json_equal(as_text(*collection[key_from_integer(1)].value()), nested_documents_k[0]));

    failures += expect(collection[key_from_integer(1)].insert(nested_documents_k[1].data()).failed());
    failures += expect(collection[key_from_integer(2)].insert(nested_documents_k[1].data()).succeeded());
    failures += expect(json_equal(as_text(*collection[key_from_integer(2)].value()), nested_documents_k[1]));

    failures += expect(
        collection[collection_key_field_t {key_from_integer(2), "/person/0/surname"}].insert(R"("Doe")").succeeded());
    failures += expect(json_equal(as_text(*collection[key_from_integer(2)].value()),
                                  R"({"person":[{"name":"Bob","age":25,"surname":"Doe"}]})"));

    failures += expect(collection[key_from_integer(1)].upsert(nested_documents_k[2].data()).succeeded());
    failures += expect(json_equal(as_text(*collection[key_from_integer(1)].value()), nested_documents_k[2]));

    failures += expect(
        collection[collection_key_field_t {key_from_integer(1), "/person"}].upsert(R"("Charles")").succeeded());
    failures += expect(
        json_equal(as_text(*collection[key_from_integer(1)].value()), R"({"person":"Charles","age":26})"));

    failures += expect(collection[collection_key_field_t {key_from_integer(1), "/weight"}].upsert("70").succeeded());
    failures += expect(
        json_equal(as_text(*collection[key_from_integer(1)].value()), R"({"person":"Charles","age":26,"weight":70})"));
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

        failures += expect(collection[key_from_integer(1)].assign(static_cast<char const*>(document.text)).succeeded());
        failures +=
            patching
                ? expect(collection[key_from_integer(1)].patch(static_cast<char const*>(modifier.text)).succeeded())
                : expect(collection[key_from_integer(1)].merge(static_cast<char const*>(modifier.text)).succeeded());
        auto maybe_value = collection[key_from_integer(1)].value();
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
std::size_t database_docs_merge_and_patch(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    auto [collection, collection_status] = database.create<docs_collection_t>("docs");
    failures += expect(collection_status.succeeded());
    if (failures != 0) return failures;

    failures += check_modification_cases(collection, document_modification_t::patch_k);
    failures += check_modification_cases(collection, document_modification_t::merge_k);
    return failures;
}

/** Gathers fields of several documents into tables, through typed headers and type-punned exports. */
std::size_t database_docs_table(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    auto [collection, collection_status] = database.create<docs_collection_t>("docs");
    failures += expect(collection_status.succeeded());
    if (failures != 0) return failures;
    char const* const alice = R"({"person":"Alice","age":27,"height":1})";
    char const* const bob = R"({"person":"Bob","age":"27","weight":2})";
    char const* const carl = R"({"person":"Carl","age":24})";
    failures += expect(collection[key_from_integer(1)].assign(alice).succeeded());
    failures += expect(collection[key_from_integer(2)].assign(bob).succeeded());
    failures += expect(collection[key_from_integer(3)].assign(carl).succeeded());
    failures += expect(json_equal(as_text(*collection[key_from_integer(1)].value()), alice));
    failures += expect(json_equal(as_text(*collection[key_from_integer(2)].value()), bob));

    // Every table below lands in one arena, which each gather rewinds for the next
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;

    // Column names
    {
        auto maybe_fields = collection[key_from_integer(1)].gist(arena);
        auto fields = *maybe_fields;
        bool has_person = false;
        bool has_height = false;
        bool has_age = false;
        bool has_weight = false;
        for (std::string_view const name : fields) {
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
        auto maybe_table = collection[key_from_integer(1)].gather(header, arena);
        auto const& table = *maybe_table;
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

        auto maybe_table = collection[key_from_integer(1)].gather(header, arena);
        auto const& table = *maybe_table;
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
        auto maybe_table =
            collection[{key_from_integer(1), key_from_integer(2), key_from_integer(3), key_from_integer(123456)}]
                .gather(header, arena);
        auto const& table = *maybe_table;
        auto ages = table.column<0>();

        failures += expect_eq(ages[0].value, 27);
        failures += expect_eq(ages[1].value, 27);
        failures += expect(ages[1].converted);
        failures += expect_eq(ages[2].value, 24);
    }

    // Single strings column
    {
        auto header = make_table_header().with<std::string_view>("age");
        auto maybe_table =
            collection[{key_from_integer(1), key_from_integer(2), key_from_integer(3), key_from_integer(123456)}]
                .gather(header, arena);
        auto const& table = *maybe_table;
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

        auto maybe_table = collection[{key_from_integer(1), key_from_integer(2), key_from_integer(3),
                                       key_from_integer(123456), key_from_integer(654321)}]
                               .gather(header, arena);
        auto const& table = *maybe_table;
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
            field_type_t {"age", ustore_docs_field_type_i32_k},
            field_type_t {"age", ustore_docs_field_type_str_k},
            field_type_t {"person", ustore_docs_field_type_str_k},
            field_type_t {"person", ustore_docs_field_type_f32_k},
            field_type_t {"height", ustore_docs_field_type_i32_k},
            field_type_t {"weight", ustore_docs_field_type_u64_k},
        }};
        table_header_view_t const header = make_table_header(columns);

        auto maybe_table = collection[{key_from_integer(1), key_from_integer(2), key_from_integer(3),
                                       key_from_integer(123456), key_from_integer(654321)}]
                               .gather(header, arena);
        auto const& table = *maybe_table;
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

/** The edge from @p source to @p target named @p id, each spelled as the key of that integer. */
constexpr edge_t edge_of(std::uint64_t source, std::uint64_t target, std::uint64_t id) noexcept {
    return {key_from_integer(source), key_from_integer(target), key_from_integer(id)};
}

/** Connects every vertex to each @c graph_connect_stride_k-th vertex after it, and answers whether every edge fit. */
[[nodiscard]] bool fill_edges(st::basic_vector<edge_t>& edges) noexcept {
    std::uint64_t edge_id = 0;
    for (std::size_t vertex_id = 0; vertex_id != graph_vertices_count_k; ++vertex_id)
        for (std::size_t connect_with = vertex_id + graph_connect_stride_k; connect_with < graph_vertices_count_k;
             connect_with += graph_connect_stride_k) {
            ++edge_id;
            edge_t edge = edge_of(vertex_id, connect_with, edge_id);
            if (st::failed(edges.push_back(std::move(edge)))) return false;
        }
    return true;
}

/** Views @p edges as the columns the graph calls take. */
edges_view_t edges_of(st::basic_vector<edge_t> const& edges) noexcept {
    return edges_view_t(edges.begin(), edges.end());
}

/** Writes disconnected vertices. */
std::size_t database_graph_write_vertices(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    failures += expect(graph_status.succeeded());
    std::array<ustore_key_t, 4> const vertices {key_from_integer(1), key_from_integer(4), key_from_integer(5),
                                                key_from_integer(2)};
    failures += expect(graph.write_vertices(strided(vertices)).succeeded());

    failures += expect(*graph.present(key_from_integer(1)));
    failures += expect(*graph.present(key_from_integer(2)));
    failures += expect(*graph.present(key_from_integer(4)));
    failures += expect(*graph.present(key_from_integer(5)));
    return failures;
}

/** Writes an edge together with its vertices. */
std::size_t database_graph_write_edge(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    failures += expect(graph_status.succeeded());
    edge_t const edge = edge_of(1, 2, 9);
    failures += expect(graph.write_edge(edge).succeeded());
    failures += expect(*graph.present(key_from_integer(1)));
    failures += expect(*graph.present(key_from_integer(2)));
    failures += expect(!*graph.present(key_from_integer(3)));

    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    auto maybe_neighbors = graph.neighbors(key_from_integer(1), arena);
    failures += expect(maybe_neighbors);
    auto const& neighbors = *maybe_neighbors;
    failures += expect_eq(neighbors.size(), 1u);
    failures += expect_eq(neighbors[0], key_from_integer(2));
    return failures;
}

/** Checks a triangle of three vertices and three directed edges, before and after removing its parts, reading every
 *  edge into @p arena. */
std::size_t check_triangle(graph_collection_t& graph, std::array<edge_t, 3>& triangle, arena_t& arena) {
    std::size_t failures = 0;
    failures += expect(*graph.present(key_from_integer(1)));
    failures += expect(*graph.present(key_from_integer(2)));
    failures += expect(!*graph.present(key_from_integer(9)));
    failures += expect(!*graph.present(key_from_integer(10)));
    failures += expect(!*graph.present(key_from_integer(1000)));

    failures += expect_eq(*graph.degree(key_from_integer(1)), 2u);
    failures += expect_eq(*graph.degree(key_from_integer(2)), 2u);
    failures += expect_eq(*graph.degree(key_from_integer(3)), 2u);
    failures += expect_eq(*graph.degree(key_from_integer(1), ustore_vertex_role_source_k), 1u);
    failures += expect_eq(*graph.degree(key_from_integer(2), ustore_vertex_role_source_k), 1u);
    failures += expect_eq(*graph.degree(key_from_integer(3), ustore_vertex_role_source_k), 1u);

    failures += expect(graph.read_edges(key_from_integer(1), arena));
    failures += expect_eq(graph.read_edges(key_from_integer(1), arena)->size(), 2u);
    failures += expect_eq(graph.read_edges(key_from_integer(1), arena, ustore_vertex_role_source_k)->size(), 1u);
    failures += expect_eq(graph.read_edges(key_from_integer(1), arena, ustore_vertex_role_target_k)->size(), 1u);

    failures += expect_eq(graph.read_edges(key_from_integer(3), arena, ustore_vertex_role_target_k)->size(), 1u);
    failures += expect_eq(graph.read_edges(key_from_integer(2), arena, ustore_vertex_role_source_k)->size(), 1u);
    failures += expect_eq((*graph.read_edges(key_from_integer(3), arena, ustore_vertex_role_target_k))[0].source,
                          key_from_integer(2));
    failures += expect_eq((*graph.read_edges(key_from_integer(3), arena, ustore_vertex_role_target_k))[0].target,
                          key_from_integer(3));
    failures += expect_eq((*graph.read_edges(key_from_integer(3), arena, ustore_vertex_role_target_k))[0].id,
                          key_from_integer(10));
    failures += expect_eq(graph.edges_between(key_from_integer(3), key_from_integer(1), arena)->size(), 1u);
    failures += expect_eq(graph.edges_between(key_from_integer(1), key_from_integer(3), arena)->size(), 0u);

    failures += expect(graph.edges());
    {
        auto present_edges = *graph.edges(ustore_vertex_role_source_k);
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
                               {{&triangle[0].source}, 1},
                               {{&triangle[0].target}, 1},
                               {{&triangle[0].id}, 1},
                           })
                           .succeeded());
    failures += expect(*graph.present(key_from_integer(1)));
    failures += expect(*graph.present(key_from_integer(2)));
    failures += expect_eq(graph.edges_between(key_from_integer(1), key_from_integer(2), arena)->size(), 0u);

    failures += expect(graph
                           .write_edges(edges_view_t {
                               {{&triangle[0].source}, 1},
                               {{&triangle[0].target}, 1},
                               {{&triangle[0].id}, 1},
                           })
                           .succeeded());
    failures += expect_eq(graph.edges_between(key_from_integer(1), key_from_integer(2), arena)->size(), 1u);

    ustore_key_t const vertex_to_remove = key_from_integer(2);
    failures += expect(graph.remove_vertex(vertex_to_remove).succeeded());
    failures += expect(!*graph.present(vertex_to_remove));
    failures += expect_eq(graph.read_edges(vertex_to_remove, arena)->size(), 0u);
    failures += expect_eq(graph.edges_between(key_from_integer(1), vertex_to_remove, arena)->size(), 0u);
    failures += expect_eq(graph.edges_between(vertex_to_remove, key_from_integer(1), arena)->size(), 0u);

    failures += expect(graph.write_edges(edges(triangle)).succeeded());
    failures += expect(*graph.present(vertex_to_remove));
    failures += expect_eq(graph.read_edges(vertex_to_remove, arena)->size(), 2u);
    failures += expect_eq(graph.edges_between(key_from_integer(1), vertex_to_remove, arena)->size(), 1u);
    failures += expect_eq(graph.edges_between(vertex_to_remove, key_from_integer(1), arena)->size(), 0u);
    return failures;
}

/** Builds a triangle one edge at a time. */
std::size_t database_graph_triangle(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    failures += expect(graph_status.succeeded());
    std::array<edge_t, 3> triangle {edge_of(1, 2, 9), edge_of(2, 3, 10), edge_of(3, 1, 11)};
    failures += expect(graph.write_edge(triangle[0]).succeeded());
    failures += expect(graph.write_edge(triangle[1]).succeeded());
    failures += expect(graph.write_edge(triangle[2]).succeeded());

    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    auto maybe_neighbors = graph.neighbors(key_from_integer(1), arena);
    failures += expect(maybe_neighbors);
    auto const& neighbors = *maybe_neighbors;
    failures += expect_eq(neighbors.size(), 2u);
    failures += expect_eq(neighbors[0], key_from_integer(2));
    failures += expect_eq(neighbors[1], key_from_integer(3));

    failures += check_triangle(graph, triangle, arena);
    return failures;
}

/** Builds a triangle in one batch, catching inconsistent concurrent updates behind one graph call. */
std::size_t database_graph_triangle_batch(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    failures += expect(graph_status.succeeded());
    std::array<edge_t, 3> triangle {edge_of(1, 2, 9), edge_of(2, 3, 10), edge_of(3, 1, 11)};
    failures += expect(graph.write_edges(edges(triangle)).succeeded());

    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    failures += check_triangle(graph, triangle, arena);
    return failures;
}

/** A transaction reading the middle of a wedge A-B-C must fail to commit once A-B changes outside it. */
case_result_t database_graph_transaction_watch(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_transactions()) return case_result_t::skipped("the engine keeps no transactions", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    failures += expect(graph_status.succeeded());
    edge_t const edge_ab = edge_of('A', 'B', 19);
    edge_t const edge_bc = edge_of('B', 'C', 31);
    failures += expect(graph.write_edge(edge_ab).succeeded());
    failures += expect(graph.write_edge(edge_bc).succeeded());

    transaction_t transaction = *database.transaction();
    auto [transaction_graph, transaction_graph_status] = transaction.find<graph_collection_t>("graph");
    failures += expect(transaction_graph_status.succeeded());
    failures += expect(transaction_graph.degree(key_from_integer('B')) == 2);
    failures += expect(transaction_graph.remove_edge(edge_bc).succeeded());
    failures += expect(graph.remove_edge(edge_ab).succeeded());

    failures += expect(transaction.commit().failed());
    return failures;
}

/** Builds a larger graph and checks every vertex degree. */
std::size_t database_graph_random_fill(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    failures += expect(graph_status.succeeded());
    st::basic_vector<edge_t> generated;
    failures += expect(fill_edges(generated));
    failures += expect(graph.write_edges(edges_of(generated)).succeeded());

    for (std::size_t index = 0; index != graph_vertices_count_k; ++index) {
        ustore_key_t const vertex_id = key_from_integer(index);
        failures += expect(graph.present(vertex_id));
        failures += expect_eq(*graph.degree(vertex_id), 9u);
    }
    return failures;
}

/** Two transactions adding edges around a shared vertex: the later commit depends on stale state and fails. */
case_result_t database_graph_conflicting_transactions(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_transactions()) return case_result_t::skipped("the engine keeps no transactions", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    failures += expect(database.create<graph_collection_t>("graph").has_value());
    transaction_t first_transaction = *database.transaction();
    auto [first_graph, first_graph_status] = first_transaction.find<graph_collection_t>("graph");
    transaction_t second_transaction = *database.transaction();
    auto [second_graph, second_graph_status] = second_transaction.find<graph_collection_t>("graph");
    failures += expect(first_graph_status.succeeded() && second_graph_status.succeeded());

    edge_t const first_edge = edge_of(4, 5, 15);
    edge_t const second_edge = edge_of(5, 6, 16);

    failures += expect(first_graph.write_edge(first_edge).succeeded());
    failures += expect(second_graph.write_edge(second_edge).succeeded());
    failures += expect(first_transaction.commit().has_value());
    failures += expect(second_transaction.commit().failed());
    return failures;
}

/** Layers a star, a pentagon and five self-loops over the same five vertices. */
std::size_t database_graph_layering_shapes(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    failures += expect(graph_status.succeeded());

    std::array<ustore_key_t, 5> const vertices {key_from_integer(1), key_from_integer(2), key_from_integer(3),
                                                key_from_integer(4), key_from_integer(5)};
    auto check_vertices = [&](ustore_vertex_degree_t degree) {
        std::size_t vertex_failures = 0;
        for (ustore_key_t const vertex_id : vertices) {
            vertex_failures += expect(*graph.present(vertex_id) == (degree != ustore_vertex_degree_missing_k));
            vertex_failures += expect_eq(*graph.degree(vertex_id), degree);
        }
        return vertex_failures;
    };

    failures += check_vertices(ustore_vertex_degree_missing_k);

    std::array<edge_t, 5> star {edge_of(1, 3, 1), edge_of(1, 4, 2), edge_of(2, 4, 3), edge_of(2, 5, 4),
                                edge_of(3, 5, 5)};
    std::array<edge_t, 5> pentagon {edge_of(1, 2, 6), edge_of(2, 3, 7), edge_of(3, 4, 8), edge_of(4, 5, 9),
                                    edge_of(5, 1, 10)};
    std::array<edge_t, 5> self_loops {edge_of(1, 1, 11), edge_of(2, 2, 12), edge_of(3, 3, 13), edge_of(4, 4, 14),
                                      edge_of(5, 5, 15)};

    failures += expect(graph.write_edges(edges(star)).succeeded());
    failures += check_vertices(2);
    failures += expect(graph.write_edges(edges(pentagon)).succeeded());
    failures += check_vertices(4);
    failures += expect(graph.remove_edges(edges(star)).succeeded());
    failures += check_vertices(2);
    failures += expect(graph.write_edges(edges(star)).succeeded());
    failures += check_vertices(4);
    failures += expect(graph.remove_edges(edges(pentagon)).succeeded());
    failures += check_vertices(2);
    failures += expect(graph.write_edges(edges(pentagon)).succeeded());
    failures += check_vertices(4);
    failures += expect(graph.write_edges(edges(self_loops)).succeeded());
    failures += check_vertices(6);
    failures += expect(graph.remove_edges(edges(star)).succeeded());
    failures += expect(graph.remove_edges(edges(pentagon)).succeeded());
    failures += check_vertices(2);
    failures += expect(graph.remove_edges(edges(self_loops)).succeeded());
    failures += check_vertices(0);
    failures += expect(graph.clear().succeeded());
    failures += check_vertices(ustore_vertex_degree_missing_k);
    return failures;
}

/** Removes vertices one by one and in a batch, each removal updating every neighbor. */
std::size_t database_graph_remove_vertices(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    failures += expect(graph_status.succeeded());
    st::basic_vector<edge_t> generated;
    failures += expect(fill_edges(generated));
    failures += expect(graph.write_edges(edges_of(generated)).succeeded());

    for (std::size_t index = 0; index != graph_vertices_count_k; ++index) {
        ustore_key_t const vertex_id = key_from_integer(index);
        failures += expect(graph.present(vertex_id));
        failures += expect(*graph.present(vertex_id));
        failures += expect(graph.remove_vertex(vertex_id).succeeded());
        failures += expect(graph.present(vertex_id));
        failures += expect(!*graph.present(vertex_id));
    }

    failures += expect(graph.write_edges(edges_of(generated)).succeeded());
    std::array<ustore_key_t, graph_vertices_count_k> vertices;
    for (std::size_t index = 0; index != vertices.size(); ++index) vertices[index] = key_from_integer(index);
    failures += expect(graph.remove_vertices(strided(vertices).immutable()).succeeded());
    for (std::size_t index = 0; index != graph_vertices_count_k; ++index) {
        ustore_key_t const vertex_id = key_from_integer(index);
        failures += expect(graph.present(vertex_id));
        failures += expect(!*graph.present(vertex_id));
    }
    return failures;
}

/** Removing a known list of edges leaves the vertices in the graph, disconnected. */
std::size_t database_graph_remove_edges_keep_vertices(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    failures += expect(graph_status.succeeded());
    st::basic_vector<edge_t> generated;
    failures += expect(fill_edges(generated));
    failures += expect(graph.write_edges(edges_of(generated)).succeeded());
    failures += expect(graph.remove_edges(edges_of(generated)).succeeded());

    for (std::size_t index = 0; index != graph_vertices_count_k; ++index) {
        ustore_key_t const vertex_id = key_from_integer(index);
        failures += expect(graph.present(vertex_id));
        failures += expect(*graph.present(vertex_id));
    }
    return failures;
}

/** Counts the edges of each vertex, then removes exactly those edges. */
std::size_t database_graph_get_vertex_edges(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    failures += expect(graph_status.succeeded());
    st::basic_vector<edge_t> generated;
    failures += expect(fill_edges(generated));
    failures += expect(graph.write_edges(edges_of(generated)).succeeded());

    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    st::basic_vector<edge_t> received;
    for (std::size_t index = 0; index != graph_vertices_count_k; ++index) {
        ustore_key_t const vertex_id = key_from_integer(index);
        auto vertex_edges = *graph.read_edges(vertex_id, arena);
        failures += expect_eq(vertex_edges.size(), 9u);
        for (std::size_t index = 0; index != vertex_edges.size(); ++index) {
            edge_t edge = vertex_edges[index];
            failures += expect(st::succeeded(received.push_back(std::move(edge))));
        }
    }
    failures += expect(graph.remove_edges(edges_of(received)).succeeded());

    for (std::size_t index = 0; index != graph_vertices_count_k; ++index) {
        ustore_key_t const vertex_id = key_from_integer(index);
        failures += expect(graph.present(vertex_id));
        failures += expect(*graph.present(vertex_id));
        failures += expect_eq(graph.read_edges(vertex_id, arena)->size(), 0u);
    }
    return failures;
}

/** Reads the degrees of many vertices in one call. */
std::size_t database_graph_degrees(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    failures += expect(graph_status.succeeded());
    std::array<ustore_key_t, graph_vertices_count_k> vertices;
    for (std::size_t index = 0; index != vertices.size(); ++index) vertices[index] = key_from_integer(index);

    st::basic_vector<edge_t> generated;
    failures += expect(fill_edges(generated));
    failures += expect(graph.write_edges(edges_of(generated)).succeeded());

    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    auto degrees = *graph.degrees(strided(vertices).immutable(), arena);
    failures += expect_eq(degrees.size(), graph_vertices_count_k);
    return failures;
}

/** Neighbors of a vertex include itself through a self-loop and both directions of its edges. */
std::size_t database_graph_neighbors(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    auto [graph, graph_status] = database.create<graph_collection_t>("graph");
    failures += expect(graph_status.succeeded());
    failures += expect(graph.write_edge(edge_of(1, 1, 17)).succeeded());
    failures += expect(graph.write_edge(edge_of(1, 2, 15)).succeeded());
    failures += expect(graph.write_edge(edge_of(2, 3, 16)).succeeded());

    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    auto maybe_first_neighbors = graph.neighbors(key_from_integer(1), arena);
    failures += expect(maybe_first_neighbors);
    auto const& first_neighbors = *maybe_first_neighbors;
    failures += expect_eq(first_neighbors.size(), 2u);
    failures += expect_eq(first_neighbors[0], key_from_integer(1));
    failures += expect_eq(first_neighbors[1], key_from_integer(2));

    auto maybe_second_neighbors = graph.neighbors(key_from_integer(2), arena);
    failures += expect(maybe_second_neighbors);
    auto const& second_neighbors = *maybe_second_neighbors;
    failures += expect_eq(second_neighbors.size(), 2u);
    failures += expect_eq(second_neighbors[0], key_from_integer(1));
    failures += expect_eq(second_neighbors[1], key_from_integer(3));
    return failures;
}

#pragma endregion Graph Modality

#pragma region Pages Modality

/** The key of the C ABI spelling the same 16 bytes as @p element. */
ustore_key_t page_key_of(element_key_t const& element) noexcept {
    ustore_key_t key;
    std::memcpy(&key, element.bytes, sizeof(key));
    return key;
}

/** The element key spelling the same 16 bytes as @p key. */
element_key_t element_key_of(ustore_key_t const& key) noexcept {
    element_key_t element;
    std::memcpy(element.bytes, &key, sizeof(key));
    return element;
}

/** One task of a page call, each verb reading the fields it takes. */
struct page_task_t {
    element_key_t element {}, parent {}, anchor {};
    std::uint64_t start = 0, length = 0;
    std::string field, value;
};

/** What a read of a missing page or element answers in @c pages_client_t::read. */
constexpr std::string_view page_absent_k = "<absent>";

/** Blocking page calls on one collection of a database, through the C ABI, joining @c transaction when set. */
struct pages_client_t {
    ustore_database_t database = nullptr;
    ustore_collection_id_t collection = ustore_collection_main_k;
    ustore_transaction_t transaction = nullptr;
    arena_t arena;

    /** Replaces the page keyed @p page with @p markup. */
    status_t import(element_key_t const& page, std::string_view markup,
                    ustore_page_encoding_t encoding = ustore_page_encoding_xml_k) {
        ustore_key_t const key = page_key_of(page);
        strided_strings<std::string_view> const values {{&markup, 0}};
        ustore_outcome_t outcome {};
        ustore_pages_import_t import {};
        import.database = database, import.outcome = &outcome, import.transaction = transaction;
        import.count = 1, import.collections = {&collection, 0}, import.keys = {&key, 0};
        import.values = values.view(), import.encoding = encoding;
        ustore_pages_import(&import);
        return status_t {outcome};
    }

    /** Applies @p tasks of @p verb to the page keyed @p page in one call, the effective keys of an insertion landing
     *  in @p effective. */
    status_t edit(modification_t verb, element_key_t const& page, std::vector<page_task_t> const& tasks,
                  std::vector<element_key_t>* effective = nullptr, bool mint = false) {
        ustore_key_t const key = page_key_of(page);
        std::vector<ustore_key_t> elements, parents, anchors;
        std::vector<ustore_length_t> starts, lengths;
        std::vector<std::string_view> fields, values;
        for (page_task_t const& task : tasks) {
            elements.push_back(page_key_of(task.element)), parents.push_back(page_key_of(task.parent));
            anchors.push_back(page_key_of(task.anchor)), starts.push_back(task.start), lengths.push_back(task.length);
            fields.push_back(task.field), values.push_back(task.value);
        }
        strided_strings<std::string_view> const fields_strings {{fields.data(), sizeof(std::string_view)}};
        strided_strings<std::string_view> const values_strings {{values.data(), sizeof(std::string_view)}};
        ustore_keys_view_t const elements_view {elements.data(), sizeof(ustore_key_t)};
        ustore_keys_view_t const parents_view {parents.data(), sizeof(ustore_key_t)};
        ustore_keys_view_t const anchors_view {anchors.data(), sizeof(ustore_key_t)};
        ustore_outcome_t outcome {};
        auto fill = [&](auto& arguments) {
            arguments.database = database, arguments.outcome = &outcome, arguments.transaction = transaction;
            arguments.count = tasks.size(), arguments.collections = {&collection, 0}, arguments.keys = {&key, 0};
        };
        switch (verb) {
        case modification_t::insert_k: {
            ustore_key_t const* landed = nullptr;
            ustore_pages_insert_t insert {};
            fill(insert);
            insert.elements = mint ? ustore_keys_view_t {} : elements_view;
            insert.parents = parents_view, insert.anchors = anchors_view;
            insert.values = values_strings.view(), insert.encoding = ustore_page_encoding_xml_k;
            insert.effective_keys = &landed, insert.arena = arena;
            ustore_pages_insert(&insert);
            if (effective && outcome.status == ustore_status_success_k)
                for (std::size_t index = 0; index != tasks.size(); ++index)
                    effective->push_back(element_key_of(landed[index]));
            break;
        }
        case modification_t::remove_k: {
            ustore_pages_remove_t remove {};
            fill(remove);
            remove.elements = elements_view;
            ustore_pages_remove(&remove);
            break;
        }
        case modification_t::move_k: {
            ustore_pages_move_t move {};
            fill(move);
            move.elements = elements_view, move.parents = parents_view, move.anchors = anchors_view;
            ustore_pages_move(&move);
            break;
        }
        case modification_t::splice_k: {
            ustore_pages_splice_t splice {};
            fill(splice);
            splice.elements = elements_view, splice.values = values_strings.view();
            splice.text_starts = {starts.data(), sizeof(ustore_length_t)};
            splice.text_lengths = {lengths.data(), sizeof(ustore_length_t)};
            ustore_pages_splice(&splice);
            break;
        }
        case modification_t::set_attributes_k: {
            ustore_pages_set_attributes_t set {};
            fill(set);
            set.elements = elements_view, set.fields = fields_strings.view(), set.values = values_strings.view();
            ustore_pages_set_attributes(&set);
            break;
        }
        case modification_t::remove_attributes_k: {
            ustore_pages_remove_attributes_t remove {};
            fill(remove);
            remove.elements = elements_view, remove.fields = fields_strings.view();
            ustore_pages_remove_attributes(&remove);
            break;
        }
        default: return {status_code_t::invalid_argument_k, "Imports go through their own call"};
        }
        return status_t {outcome};
    }

    /** The rendering of @p element of the page keyed @p page, the whole page for Nil, @c page_absent_k where missing,
     *  or the status name of a failure. */
    std::string read(element_key_t const& page, element_key_t const& element = {},
                     ustore_page_encoding_t encoding = ustore_page_encoding_xml_k, ustore_view_t view = nullptr) {
        ustore_key_t const key = page_key_of(page);
        ustore_key_t const wanted = page_key_of(element);
        ustore_octet_t const* presences = nullptr;
        ustore_strings_view_t values {};
        ustore_outcome_t outcome {};
        ustore_pages_read_t read {};
        read.database = database, read.outcome = &outcome, read.view = view;
        read.count = 1, read.collections = {&collection, 0}, read.keys = {&key, 0}, read.elements = {&wanted, 0};
        read.presences = &presences, read.values = &values, read.arena = arena, read.outputs_encoding = encoding;
        ustore_pages_read(&read);
        if (outcome.status != ustore_status_success_k) return ustore_status_name(outcome.status);
        if (!(presences[0] & 1)) return std::string(page_absent_k);
        return std::string(string_of(tape_string(values, 0)));
    }
};

/** Whether the engine behind @p database serves the pages calls, as every engine with blobs does. */
bool serves_pages(ustore_database_t database) {
    pages_client_t probe {database};
    auto [arena, arena_status] = arena_t::make();
    if (arena_status.failed()) return false;
    probe.arena = std::move(arena);
    return probe.read(mint_key(1, 1, 1)) != std::string_view {"unsupported"};
}

/** Creates a collection named @p name holding pages. */
expected<ustore_collection_id_t> create_pages_collection(ustore_database_t database, std::string_view name) {
    ustore_collection_id_t created {};
    ustore_outcome_t outcome {};
    ustore_collection_create_t create {};
    create.database = database, create.outcome = &outcome, create.name = bytes_of(name);
    create.modality = ustore_modality_pages_k, create.collection = &created;
    ustore_collection_create(&create);
    if (status_t const status {outcome}; status.failed()) return status;
    return created;
}

/** Generates random page calls against a model, mostly aimed at elements the stream gave keys to, sometimes at removed
 *  or unknown ones, so the forgiving rules run too. Elements whose keys the engine mints get placeholder keys in the
 *  model, which no call ever names. */
class page_stream_t {
    std::mt19937_64& random_;
    std::uint64_t stream_;
    std::uint32_t given_ = 0, placeholders_ = 0;
    std::vector<element_key_t> given_keys_;

  public:
    element_key_t page;
    model_page_t model;

    page_stream_t(std::mt19937_64& random, std::uint64_t stream) noexcept
        : random_(random), stream_(stream), page(element_key_of(key_from_integer(1000 + stream))) {}

    std::string text(std::size_t longest, bool for_comment = false) {
        static constexpr std::string_view alphabet = "abcxyz &<>\"'";
        std::string result(random_() % (longest + 1), ' ');
        for (char& character : result) character = alphabet[random_() % alphabet.size()];
        // A comment body may hold neither `--` nor `-->`, and this alphabet spells neither
        if (for_comment)
            for (char& character : result)
                if (character == '&') character = 'q';
        return result;
    }

    element_key_t fresh_given() {
        element_key_t const key = element_key_of(key_from_integer(1'000'000 * (stream_ + 1) + ++given_));
        given_keys_.push_back(key);
        return key;
    }

    element_key_t placeholder() { return mint_key(~std::uint64_t(0), std::uint32_t(stream_), ++placeholders_); }

    /** A key the stream gave out, mostly a live one, sometimes a retired one, the page key or an unknown one. */
    element_key_t any_key(bool elements_only) {
        std::vector<element_key_t> live;
        for (element_key_t const& key : given_keys_)
            if (model.live(key) && (!elements_only || model.elements.at(key).kind == element_kind_t::element_k))
                live.push_back(key);
        std::uint64_t const roll = random_() % 10;
        if (roll == 0 && !given_keys_.empty()) return given_keys_[random_() % given_keys_.size()];
        if (roll == 1) return element_key_of(key_from_integer(77));
        if (roll == 2 || live.empty()) return page;
        return live[random_() % live.size()];
    }

    std::string attribute_name() {
        static constexpr std::string_view names[] = {"id", "class", "href", "x"};
        return std::string(names[random_() % 4]);
    }

    /** One random node of an insertion keyed @p key, spelled as XML in @p markup. */
    node_spec_t node(element_key_t const& key, std::string& markup, bool text_allowed = true) {
        node_spec_t spec;
        spec.key = key;
        std::uint64_t const roll = random_() % 6;
        if (roll == 0) {
            spec.kind = element_kind_t::comment_k, spec.text = text(4, true);
            markup += "<!--" + spec.text + "-->";
            return spec;
        }
        if (roll == 1 && text_allowed) {
            spec.kind = element_kind_t::text_k, spec.text = "t" + text(4);
            markup += model_page_t::escaped(spec.text, false);
            return spec;
        }
        spec.name = random_() % 2 ? "p" : "row";
        for (std::size_t attribute = random_() % 3; attribute; --attribute) {
            std::string name = attribute_name();
            bool taken = false;
            for (auto const& existing : spec.attributes) taken |= existing.first == name;
            if (!taken) spec.attributes.emplace_back(name, text(3));
        }
        spec.text = text(5);
        model_page_t single;
        single.exists = true, single.root = page, single.elements[page] = {};
        single.graft(page, 0, {spec});
        markup += single.render_xml(key);
        return spec;
    }

    /** A random nested fragment of elements with leading text to import, spelled as XML in @p markup. */
    std::vector<node_spec_t> import_fragment(std::string& markup) {
        std::vector<node_spec_t> nodes;
        std::size_t const count = random_() % 7;
        std::size_t depth_limit = 1;
        for (std::size_t index = 0; index != count; ++index) {
            node_spec_t spec;
            spec.key = placeholder();
            spec.depth = random_() % depth_limit + 1;
            spec.name = random_() % 2 ? "p" : "s:t";
            if (random_() % 2) spec.attributes.emplace_back(attribute_name(), text(3));
            spec.text = text(4);
            depth_limit = spec.depth + 1;
            nodes.push_back(spec);
        }
        model_page_t staged;
        staged.exists = true, staged.root = page, staged.elements[page] = {};
        staged.graft(page, 0, nodes);
        markup = staged.render_xml(page);
        return nodes;
    }
};

/** Random streams of every page call agree with the reference model of `test/pages/model.hpp` after every call, read
 *  whole, as text and by subtree, and a snapshot keeps reading a page as it was. */
case_result_t database_pages_random_streams(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    if (!serves_pages(database)) return case_result_t::skipped("the engine serves no pages yet", failures);
    bool const snapshots = database.supports_snapshots();

    std::mt19937_64 random(42);
    for (std::uint64_t stream_index = 0; stream_index != 8; ++stream_index) {
        pages_client_t client {database};
        auto [arena, arena_status] = arena_t::make();
        failures += expect(arena_status.succeeded());
        if (failures) return failures;
        client.arena = std::move(arena);
        auto [created, create_status] = create_pages_collection(database, fmt::format("pages-{}", stream_index));
        failures += expect(create_status.succeeded());
        if (failures) return failures;
        client.collection = created;

        page_stream_t stream(random, stream_index);
        ustore_snapshot_t snapshot = nullptr;
        std::string snapshot_rendering;
        for (std::size_t call = 0; call != 120 && !failures; ++call) {
            if (snapshots && call == 60) {
                ustore_outcome_t outcome {};
                ustore_snapshot_init_t init {};
                init.snapshot = &snapshot, init.database = database, init.outcome = &outcome;
                ustore_snapshot_init(&init);
                failures += expect_success(outcome);
                snapshot_rendering = client.read(stream.page);
            }

            std::uint64_t const roll = random() % 100;
            std::vector<model_edit_t> edits;
            std::vector<page_task_t> tasks;
            modification_t verb = modification_t::import_k;
            std::size_t const count = random() % 3 + 1;
            status_t status;
            bool minted = false;
            std::vector<element_key_t> effective;

            if (!stream.model.exists || roll < 4) {
                model_edit_t edit;
                edit.modification = modification_t::import_k, edit.element = stream.page;
                std::string markup;
                if (random() % 4 == 0) {
                    markup = stream.text(6);
                    if (!markup.empty()) {
                        node_spec_t spec;
                        spec.key = stream.placeholder(), spec.kind = element_kind_t::text_k, spec.text = markup;
                        edit.fragment.push_back(spec);
                    }
                    status = client.import(stream.page, markup, ustore_page_encoding_text_k);
                }
                else {
                    edit.fragment = stream.import_fragment(markup);
                    status = client.import(stream.page, markup);
                }
                edits.push_back(edit);
            }
            else if (roll < 6) {
                verb = modification_t::remove_k;
                model_edit_t edit;
                edit.modification = modification_t::remove_k;
                edits.push_back(edit);
                tasks.push_back({});
                status = client.edit(verb, stream.page, tasks);
            }
            else {
                minted = roll < 40 && random() % 8 == 0;
                verb = roll < 40   ? modification_t::insert_k
                       : roll < 50 ? modification_t::remove_k
                       : roll < 62 ? modification_t::move_k
                       : roll < 80 ? modification_t::splice_k
                       : roll < 93 ? modification_t::set_attributes_k
                                   : modification_t::remove_attributes_k;
                for (std::size_t task = 0; task != count; ++task) {
                    model_edit_t edit;
                    page_task_t call_task;
                    edit.modification = verb;
                    switch (verb) {
                    case modification_t::insert_k: {
                        element_key_t const first = minted               ? stream.placeholder()
                                                    : random() % 10 == 0 ? stream.any_key(false)
                                                                         : stream.fresh_given();
                        edit.fragment.push_back(stream.node(first, call_task.value));
                        if (random() % 4 == 0)
                            edit.fragment.push_back(stream.node(stream.placeholder(), call_task.value, false));
                        call_task.element = first;
                        call_task.parent = edit.parent = random() % 4 == 0 ? nil_key_k : stream.any_key(true);
                        call_task.anchor = edit.anchor = random() % 3 == 0 ? nil_key_k : stream.any_key(false);
                        break;
                    }
                    case modification_t::remove_k: call_task.element = edit.element = stream.any_key(false); break;
                    case modification_t::move_k:
                        call_task.element = edit.element = stream.any_key(false);
                        call_task.parent = edit.parent = random() % 4 == 0 ? nil_key_k : stream.any_key(true);
                        call_task.anchor = edit.anchor = random() % 3 == 0 ? nil_key_k : stream.any_key(false);
                        break;
                    case modification_t::splice_k: {
                        call_task.element = edit.element = stream.any_key(false);
                        std::size_t const length = stream.model.live(edit.element)
                                                       ? stream.model.elements.at(edit.element).text.size()
                                                       : 4;
                        call_task.start = edit.text_start = random() % (length + 3);
                        call_task.length = edit.text_length = random() % 4;
                        call_task.value = edit.value = stream.text(4);
                        break;
                    }
                    case modification_t::set_attributes_k:
                        call_task.element = edit.element = stream.any_key(false);
                        call_task.field = edit.field = stream.attribute_name();
                        call_task.value = edit.value = stream.text(3);
                        break;
                    default:
                        call_task.element = edit.element = stream.any_key(false);
                        call_task.field = edit.field = stream.attribute_name();
                        break;
                    }
                    edits.push_back(std::move(edit));
                    tasks.push_back(std::move(call_task));
                }
                status = client.edit(verb, stream.page, tasks, &effective, minted);
            }

            failures += expect_status(status, status_code_t::success_k);
            if (status.failed()) break;
            for (std::size_t index = 0; index != effective.size(); ++index)
                if (!minted) failures += expect(effective[index] == tasks[index].element);
            for (model_edit_t const& edit : edits) stream.model.apply(edit);

            std::string const wanted = stream.model.exists ? stream.model.render_xml(stream.page)
                                                           : std::string(page_absent_k);
            failures += expect_eq(client.read(stream.page), wanted);
            std::string const wanted_text = stream.model.exists ? stream.model.render_text(stream.page)
                                                                : std::string(page_absent_k);
            failures += expect_eq(client.read(stream.page, {}, ustore_page_encoding_text_k), wanted_text);
            element_key_t const probed = stream.any_key(false);
            std::string const wanted_subtree = stream.model.exists && stream.model.live(probed)
                                                   ? stream.model.render_xml(probed)
                                                   : std::string(page_absent_k);
            failures += expect_eq(client.read(stream.page, probed), wanted_subtree);
        }

        // Editing a removed page answers that it is missing
        if (!stream.model.exists && !failures) {
            failures += expect_status(client.edit(modification_t::splice_k, stream.page, {{}}),
                                      status_code_t::not_found_k);
        }

        // A snapshot keeps reading the page as it was when it was taken
        if (snapshot) {
            failures += expect_eq(
                client.read(stream.page, {}, ustore_page_encoding_xml_k, ustore_snapshot_view(snapshot)),
                snapshot_rendering);
            ustore_snapshot_free(snapshot);
        }
        if (failures) {
            fmt::println(stderr, "  pages stream {} diverged", stream_index);
            return failures;
        }
    }
    return failures;
}

/** Edits of one page in a transaction stay invisible until it commits, and a concurrent transaction that edited the
 *  same page either fails to commit, where every edit watches the page it rewrites, or lands beside the first, where
 *  the engine keeps a page as its edits. */
case_result_t database_pages_transactions(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    if (!serves_pages(database)) return case_result_t::skipped("the engine serves no pages yet", failures);
    if (!database.supports_transactions()) return case_result_t::skipped("the engine keeps no transactions", failures);

    auto [pages, pages_status] = create_pages_collection(database, "pages");
    pages_client_t head {database, pages};
    auto [arena, arena_status] = arena_t::make();
    failures += expect(pages_status.succeeded() && arena_status.succeeded());
    if (failures) return failures;
    head.arena = std::move(arena);
    element_key_t const page = element_key_of(key_from_integer(7));
    failures += expect(head.import(page, "<doc><p>one</p></doc>").succeeded());

    auto [first, first_status] = database.transaction();
    auto [second, second_status] = database.transaction();
    failures += expect(first_status.succeeded() && second_status.succeeded());
    if (failures) return failures;
    pages_client_t first_client {database, pages, first.transaction()};
    pages_client_t second_client {database, pages, second.transaction()};
    auto [first_arena, first_arena_status] = arena_t::make();
    auto [second_arena, second_arena_status] = arena_t::make();
    failures += expect(first_arena_status.succeeded() && second_arena_status.succeeded());
    if (failures) return failures;
    first_client.arena = std::move(first_arena), second_client.arena = std::move(second_arena);

    page_task_t inserted;
    inserted.value = "<p>two</p>";
    std::vector<element_key_t> effective;
    failures += expect(first_client.edit(modification_t::insert_k, page, {inserted}, &effective, true).succeeded());
    failures += expect_eq(effective.size(), 1u);
    failures += expect(!effective.empty() && !effective[0].is_nil());
    page_task_t spliced;
    spliced.element = page, spliced.value = "lead";
    failures += expect(second_client.edit(modification_t::splice_k, page, {spliced}).succeeded());

    failures += expect_eq(head.read(page), std::string("<doc><p>one</p></doc>"));
    failures += expect_eq(first_client.read(page, {}, ustore_page_encoding_xml_k,
                                            ustore_transaction_view(first.transaction(), ustore_watch_none_k)),
                          std::string("<p>two</p><doc><p>one</p></doc>"));
    failures += expect(first.commit().has_value());
    failures += expect_eq(head.read(page), std::string("<p>two</p><doc><p>one</p></doc>"));

    // A page rewritten whole conflicts, while a page kept as its edits lands both, folded in commit order
    bool const both_landed = second.commit().has_value();
    failures += expect_eq(head.read(page), std::string(both_landed ? "lead<p>two</p><doc><p>one</p></doc>"
                                                                   : "<p>two</p><doc><p>one</p></doc>"));
    return failures;
}

/** A failing task leaves every page of its call untouched, and a page is one value, its current shape, which a removal
 *  of the page takes away. */
case_result_t database_pages_all_or_nothing(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    if (!serves_pages(database)) return case_result_t::skipped("the engine serves no pages yet", failures);

    auto [pages, pages_status] = create_pages_collection(database, "pages");
    pages_client_t client {database, pages};
    auto [arena, arena_status] = arena_t::make();
    failures += expect(pages_status.succeeded() && arena_status.succeeded());
    if (failures) return failures;
    client.arena = std::move(arena);
    auto stored_keys = [&] {
        std::vector<ustore_key_t> keys;
        auto [stream, stream_status] = keys_stream_t::make(database.receiver(), pages, smallest_key_k, 256,
                                                           ustore_modality_pages_k);
        failures += expect(stream_status.succeeded());
        for (; stream_status.succeeded() && !stream.is_end(); ++stream) keys.push_back(stream.key());
        return keys;
    };
    element_key_t const page = element_key_of(key_from_integer(8));
    failures += expect(client.import(page, "<a>x</a>").succeeded());
    std::vector<page_task_t> tasks(3);
    tasks[0].value = "<b/>", tasks[1].value = "<c>", tasks[2].value = "<d/>";
    failures += expect_status(client.edit(modification_t::insert_k, page, tasks, nullptr, true),
                              status_code_t::malformed_k);
    failures += expect_eq(client.read(page), std::string("<a>x</a>"));
    failures += expect(stored_keys() == std::vector<ustore_key_t> {key_from_integer(8)});

    // Removing the page by a Nil element empties it until the next import, and the page key empties only its root
    failures += expect(client.edit(modification_t::remove_k, page, {{}}).succeeded());
    failures += expect_eq(client.read(page), std::string(page_absent_k));
    failures += expect(stored_keys().empty());
    failures += expect(client.import(page, "<a>x</a><b/>").succeeded());
    page_task_t root;
    root.element = page;
    failures += expect(client.edit(modification_t::remove_k, page, {root}).succeeded());
    failures += expect_eq(client.read(page), std::string(""));
    failures += expect(stored_keys() == std::vector<ustore_key_t> {key_from_integer(8)});
    return failures;
}

#pragma endregion Pages Modality

#pragma region Executors

/** Reports progress once, standing in for a long scan whose progress callback a case holds open. */
status_t scan_reporting_progress(ustore_blobs_scan_t const& arguments, view_t const&) noexcept {
    if (arguments.progress && arguments.progress(0, 1, arguments.progress_state) == ustore_cancel_k)
        return {status_code_t::cancelled_k, "The progress callback stopped the scan"};
    return {};
}

/** Leaves the stand-in database to the case holding it. */
void leave_database(ustore_database_t) noexcept {}

/** The stand-in engine's only database verb, which leaves the database to the case. */
database_verbs_t const progress_database_verbs_k {.database_free = &leave_database};

/** The stand-in engine's only blobs verb, a scan reporting its progress once. */
blobs_verbs_t const progress_blobs_verbs_k {.scan = &scan_reporting_progress};

/** An engine serving only scans, each reporting its progress once, so a call stays unfinished while a case needs. */
engine_t const progress_engine_k {.database = &progress_database_verbs_k, .blobs = &progress_blobs_verbs_k};

/** How far a scan held inside its progress callback got. */
enum class hold_stage_t : std::uint32_t {
    submitted_k,
    entered_k,
    released_k,
};

/** A deferred scan on the stand-in engine, its callback holding it until the case releases it, and everything its
 *  arguments point to, while the arguments themselves die at submission. */
struct held_scan_t {
    handle_t database {&progress_engine_k};
    hold_stage_t stage = hold_stage_t::submitted_k;
    ustore_outcome_t outcome {};
    ustore_key_t start_key {};
    ustore_length_t const* offsets = nullptr;
    ustore_key_t const* keys = nullptr;
    arena_t arena;
};

/** Holds the scan until released, so every deferred call submitted behind it stays unfinished. */
ustore_continuation_t hold_until_released(ustore_size_t, ustore_size_t, void* state) noexcept {
    std::atomic_ref<hold_stage_t> const stage {static_cast<held_scan_t*>(state)->stage};
    stage.store(hold_stage_t::entered_k, std::memory_order_release);
    stage.wait(hold_stage_t::entered_k, std::memory_order_acquire);
    return ustore_continue_k;
}

/** Submits @p held to @p executor and waits until its callback holds it. */
std::size_t submit_held(held_scan_t& held, ustore_executor_t executor) noexcept {
    auto [arena, arena_status] = arena_t::make(executor);
    if (arena_status.failed()) return expect(arena_status.succeeded());
    held.arena = std::move(arena);
    ustore_blobs_scan_t scan {
        .database = c_handle<ustore_database_t>(&held.database),
        .executor = executor,
        .outcome = &held.outcome,
        .completion = ustore_completion_deferred_k,
        .count = 1,
        .start_keys = {&held.start_key, 0},
        .offsets = &held.offsets,
        .keys = &held.keys,
        .arena = held.arena,
        .progress = &hold_until_released,
        .progress_state = &held,
    };
    ustore_blobs_scan(&scan);
    std::atomic_ref<hold_stage_t> const stage {held.stage};
    return expect(
        spin_until([&stage]() noexcept { return stage.load(std::memory_order_acquire) == hold_stage_t::entered_k; }));
}

/** Lets the callback of @p held return, and waits until the scan finishes. */
std::size_t release_held(held_scan_t& held) noexcept {
    std::atomic_ref<hold_stage_t> const stage {held.stage};
    stage.store(hold_stage_t::released_k, std::memory_order_release);
    stage.notify_all();
    return expect(ustore_outcome_wait(&held.outcome) == ustore_status_success_k);
}

/** Proxies on an executor read and write as on the calling thread, at the head, in a transaction and at a snapshot. */
std::size_t executor_on_receivers(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(executor_status.succeeded());
    if (failures != 0) return failures;

    triplet_t triplet;
    blobs_collection_t collection = database.on(executor).main();
    auto reference = collection[triplet.keys];
    failures += round_trip(reference, triplet);

    if (database.supports_transactions()) {
        transaction_t transaction = *database.on(executor).transaction();
        transaction_ref_t transaction_on_executor = transaction.on(executor);
        auto transaction_reference = transaction_on_executor[triplet.keys];
        failures += check_equalities(transaction_reference, triplet);
        failures += expect(transaction_on_executor.main().at(triplet.keys).remove().succeeded());
        failures += expect(transaction_on_executor.commit().has_value());
        failures += check_length(reference, missing_length_k);
    }
    if (database.supports_snapshots()) {
        failures += round_trip(reference, triplet);
        snapshot_t snapshot = *database.on(executor).snapshot();
        snapshot_ref_t snapshot_on_executor = snapshot.on(executor);
        auto snapshot_reference = snapshot_on_executor[triplet.keys];
        failures += check_equalities(snapshot_reference, triplet);
    }
    failures += expect(executor.drain(ustore_mood_spinning_k).succeeded());
    return failures;
}

/** A deferred write through a transaction reaches the head only once the deferred commit finishes, which answers a
 *  stamp no lower than the one the deferred stage read at. */
case_result_t executor_deferred_commit(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_transactions()) return case_result_t::skipped("the engine keeps no transactions", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(executor_status.succeeded());
    if (failures != 0) return failures;

    std::array<ustore_key_t, 2> const keys {key_from_integer(75), key_from_integer(76)};
    values_tape_t values;
    failures += fill_tape(values, {"left", "right"});

    transaction_t transaction = *database.transaction();
    deferred_transaction_t deferred_transaction = transaction.deferred(executor);
    failures += expect(deferred_transaction.main().write(keys, std::move(values)).wait().succeeded());
    blobs_collection_t head = database.main();
    failures += expect(!*head[keys[0]].present());

    auto [read_stamp, stage_status] = deferred_transaction.stage().wait();
    failures += expect(stage_status.succeeded());
    auto [commit_stamp, commit_status] = deferred_transaction.commit().wait();
    failures += expect(commit_status.succeeded());
    failures += expect(static_cast<ustore_stamp_t>(read_stamp) <= static_cast<ustore_stamp_t>(commit_stamp));
    failures += expect(*head[keys[0]].present() && *head[keys[1]].present());
    return failures;
}

/** A deferred document write and then deferred reads hand back the documents whole, one field of each as text, and a
 *  gathered column whose table outlives every move of its answer, and a deferred removal leaves the keys missing. */
std::size_t executor_deferred_docs(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(executor_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;

    failures += expect(database.create<docs_collection_t>("docs").has_value());
    auto [collection, collection_status] = database.deferred(executor).find<docs_collection_t>("docs");
    failures += expect(collection_status.succeeded());
    if (failures != 0) return failures;

    std::array<ustore_key_t, 2> const keys {key_from_integer(81), key_from_integer(82)};
    values_tape_t documents;
    failures += fill_tape(documents, {flat_documents_k[0], flat_documents_k[1]});
    failures += expect(collection.write(keys, std::move(documents)).wait().succeeded());

    auto [read, read_status] = collection.read(keys, arena).wait();
    failures += expect(read_status.succeeded());
    failures += expect_eq(read.size(), keys.size());
    for (std::size_t index = 0; index != read.size(); ++index)
        failures += expect(json_equal(as_text(read[index]), flat_documents_k[index]));

    values_tape_t person;
    failures += fill_tape(person, {"person"});
    auto [people, people_status] = collection.read(keys, arena, std::move(person), ustore_docs_field_type_str_k).wait();
    failures += expect(people_status.succeeded());
    failures += expect_eq(people.size(), keys.size());
    if (people.size() == keys.size()) {
        failures += expect_eq(as_text(people[0]), std::string_view("Alice"));
        failures += expect_eq(as_text(people[1]), std::string_view("Bob"));
    }

    values_tape_t age;
    failures += fill_tape(age, {"age"});
    auto [gathered, gather_status] =
        collection.gather(keys, std::move(age), std::array {ustore_docs_field_type_i32_k}, arena).wait();
    failures += expect(gather_status.succeeded());
    docs_gather_t::finished_t const moved = std::move(gathered);
    failures += expect_eq(moved.table.rows(), keys.size());
    failures += expect_eq(moved.table.columns(), 1u);
    if (moved.table.rows() == keys.size() && moved.table.columns() == 1) {
        auto const ages = moved.table.column(0).as<i32_t>();
        strided_span<std::string_view const> const names = moved.table.header().fields();
        strided_span<ustore_key_t const> const rows = moved.table.index().keys();
        failures += expect(ages[0].valid && ages[1].valid);
        failures += expect_eq(ages[0].value, 24);
        failures += expect_eq(ages[1].value, 25);
        failures += expect_eq(names[0], std::string_view("age"));
        failures += expect(rows[1] == keys[1]);
    }

    failures += expect(collection.remove(keys).wait().succeeded());
    auto [removed, removed_status] = collection.read(keys, arena).wait();
    failures += expect(removed_status.succeeded());
    failures += expect_eq(removed.size(), keys.size());
    for (std::size_t index = 0; index != removed.size(); ++index) failures += expect(!removed[index]);
    return failures;
}

/** A deferred write of edges and then deferred reads hand back the edges every vertex leaves, split by the offsets with
 *  none for a missing vertex between two present ones, and the degree of every vertex. */
case_result_t executor_deferred_graph(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_collections()) return case_result_t::skipped("the engine keeps no collections", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(executor_status.succeeded());
    failures += expect(database.create<graph_collection_t>("graph").has_value());
    auto [graph, graph_status] = database.deferred(executor).find<graph_collection_t>("graph");
    failures += expect(graph_status.succeeded());
    if (failures != 0) return failures;

    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    std::array<edge_t, 3> const triangle {edge_of(1, 2, 9), edge_of(2, 3, 10), edge_of(3, 1, 11)};
    failures += expect(graph.write_edges(triangle).wait().succeeded());

    std::array<ustore_key_t, 3> const vertices {key_from_integer(1), key_from_integer(4), key_from_integer(2)};
    auto [read, read_status] = graph.read_edges(vertices, arena, ustore_vertex_role_source_k).wait();
    failures += expect(read_status.succeeded());
    failures += expect_eq(read.size(), 2u);
    failures += expect_eq(read.offsets.size(), vertices.size() + 1);
    if (read.offsets.size() == vertices.size() + 1) {
        edges_view_t const leaving_first = read.of_vertex(0), leaving_second = read.of_vertex(2);
        failures += expect(leaving_first.size() == 1 && leaving_first[0] == triangle[0]);
        failures += expect_eq(read.of_vertex(1).size(), 0u);
        failures += expect(leaving_second.size() == 1 && leaving_second[0] == triangle[1]);
    }

    auto [degrees, degrees_status] = graph.degrees(std::array {key_from_integer(1), key_from_integer(4)}, arena).wait();
    failures += expect(degrees_status.succeeded());
    failures += expect_eq(degrees.size(), 2u);
    if (degrees.size() == 2) {
        failures += expect_eq(degrees[0], 2u);
        failures += expect_eq(degrees[1], ustore_vertex_degree_missing_k);
    }
    return failures;
}

/** A deferred write of strings and then deferred reads hand back their keys, the nil UUID for a string never written,
 *  and a page of strings for every prefix. */
case_result_t executor_deferred_strings(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_collections()) return case_result_t::skipped("the engine keeps no collections", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(executor_status.succeeded());
    failures += expect(database.create<strings_collection_t>("words").has_value());
    auto [words, words_status] = database.deferred(executor).find<strings_collection_t>("words");
    failures += expect(words_status.succeeded());
    if (failures != 0) return failures;

    values_tape_t texts;
    failures += fill_tape(texts, {"apple", "apricot", "banana"});
    std::array<ustore_key_t, 3> const keys {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
    failures += expect(words.write(std::move(texts), keys).wait().succeeded());

    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    values_tape_t asked;
    failures += fill_tape(asked, {"banana", "cherry", "apple"});
    auto [read, read_status] = words.read(std::move(asked), arena).wait();
    failures += expect(read_status.succeeded());
    failures += expect_eq(read.size(), 3u);
    if (read.size() == 3) {
        failures += expect_eq(read[0], keys[2]);
        failures += expect_eq(read[1], ustore_key_t {});
        failures += expect_eq(read[2], keys[0]);
    }

    values_tape_t prefixes;
    failures += fill_tape(prefixes, {"ap", "b"});
    auto [scanned, scan_status] = words.scan(std::move(prefixes), 4, arena).wait();
    failures += expect(scan_status.succeeded());
    failures += expect_eq(scanned.pages.size(), 3u);
    failures += expect_eq(scanned.strings.size(), 3u);
    if (scanned.pages.size() == 3 && scanned.strings.size() == 3) {
        failures += expect_eq(scanned.pages[1] - scanned.pages[0], 2u);
        failures += expect_eq(scanned.strings[0], sz::string_view_t("apple"));
        failures += expect_eq(scanned.strings[1], sz::string_view_t("apricot"));
        failures += expect_eq(scanned.strings[2], sz::string_view_t("banana"));
    }
    return failures;
}

/** A database opened blocking on an executor keeps a write, and a deferred init submitted behind an unfinished call
 *  stays deferred until that call finishes, then opens the database holding that write. */
case_result_t executor_deferred_open(config_t const& config) {
    if (!config.directory()) return case_result_t::skipped(needs_directory_k);
    std::size_t failures = 0;
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(executor_status.succeeded());
    if (failures != 0) return failures;

    ustore_key_t const key = key_from_integer(91);
    {
        ustore_database_t opened = nullptr;
        ustore_outcome_t outcome {};
        open_engine(config.options(), &opened, &outcome, executor);
        failures += expect_success(outcome);
        if (failures != 0) return failures;
        database_t database {opened};
        blobs_collection_t head = database.main();
        failures += expect(head[key].assign("opened").succeeded());
    }

    // The init waits behind the held scan, so it stays deferred until the scan is released
    held_scan_t held;
    failures += submit_held(held, executor);
    ustore_database_t reopened = nullptr;
    ustore_outcome_t outcome {};
    open_engine(config.options(), &reopened, &outcome, executor, ustore_completion_deferred_k);
    failures += expect(ustore_outcome_status(&outcome) == ustore_status_deferred_k);
    failures += release_held(held);
    ustore_outcome_wait(&outcome);
    failures += expect_success(outcome);
    if (failures != 0) return failures;
    database_t database {reopened};
    blobs_collection_t head = database.main();
    failures += expect(head[key].value() == "opened");
    return failures;
}

/** A deferred call finding the deferred table full finishes contended at once, and so does a call on a transaction
 *  that an unfinished deferred call reads through; both succeed once the executor drains. */
std::size_t executor_contended(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [narrow, narrow_status] = executor_handle_t::for_ram({.threads = 2, .deferred_limit = 1});
    failures += expect(narrow_status.succeeded());
    if (failures != 0) return failures;

    auto [arena, arena_status] = arena_t::make(narrow);
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    std::array<ustore_key_t, 1> const keys {key_from_integer(77)};
    deferred_blobs_collection_t collection = database.deferred(narrow).main();
    held_scan_t held;
    failures += submit_held(held, narrow);
    deferred<blobs_read_t> refused = collection.read(keys, arena);
    failures += expect(refused.status() == status_code_t::contended_k);
    failures += expect_status(std::move(refused).wait(), status_code_t::contended_k);
    failures += release_held(held);
    failures += expect(narrow.drain().succeeded());
    failures += expect(collection.read(keys, arena).wait().has_value());
    if (!database.supports_transactions()) return failures;

    auto [wide, wide_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(wide_status.succeeded());
    if (failures != 0) return failures;
    auto [wide_arena, wide_arena_status] = arena_t::make(wide);
    failures += expect(wide_arena_status.succeeded());
    if (failures != 0) return failures;
    transaction_t transaction = *database.transaction();
    held_scan_t held_again;
    failures += submit_held(held_again, wide);
    deferred<blobs_read_t> reading = transaction.deferred(wide).main().read(keys, wide_arena);
    failures += expect_status(transaction.main().at(keys[0]).assign("value"), status_code_t::contended_k);
    failures += release_held(held_again);
    failures += expect(std::move(reading).wait().has_value());
    failures += expect(transaction.main().at(keys[0]).assign("value").succeeded());
    return failures;
}

#pragma endregion Executors

int main(int argc, char** argv) {
    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    auto const maybe_test_directory = directory_t::make(options.directory);
    if (!maybe_test_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", options.directory);
        return 1;
    }
    char const* const directory = maybe_test_directory->path();
    if (directory) fmt::println("Will work in directory: {}", directory);
    else fmt::println("Will work with default configuration");
    config_t const config {options.engine, directory};

    report(totals, "ranges.strided_postfix", strided_iterator_postfix_steps);

    report(totals, "database.open_clear_close", database_open_clear_close, config);
    report(totals, "database.clear_by_clearing_db", database_clear_collection_by_clearing_database, config);
    report(totals, "database.describe", database_describe, config);
    report(totals, "database.clear_modes", database_clear_modes, config);
    report(totals, "database.compact", database_compact, config);
    report(totals, "database.overwrite_with_step", database_overwrite_with_step, config);
    if (directory) report(totals, "database.persistency", database_persistency, config);
    else report(totals, "database.persistency", case_result_t::skipped(needs_directory_k));
    report(totals, "database.named_collections", database_named_collections, config);
    report(totals, "database.named_collections_race", database_named_collections_race, config);
    report(totals, "database.named_collections_list", database_named_collections_list, config);
    report(totals, "database.collections_resolve", database_collections_resolve, config);
    report(totals, "database.clear_values", database_clear_values, config);
    report(totals, "database.presences", database_presences, config);
    report(totals, "database.arena_reuse", database_arena_reuse, config);
    report(totals, "database.scan", database_scan, config);
    report(totals, "database.batch_scan", database_batch_scan, config);
    report(totals, "database.transaction_read_committed", database_transaction_read_committed, config);
    report(totals, "database.snapshot_isolation", database_transaction_snapshot_isolation, config);
    report(totals, "database.transaction_with_snapshot", database_transaction_with_snapshot, config);
    if (directory) report(totals, "database.save_snapshot", database_save_snapshot, config);
    else report(totals, "database.save_snapshot", case_result_t::skipped(needs_directory_k));
    if (directory) report(totals, "database.save_progress", database_save_progress, config);
    else report(totals, "database.save_progress", case_result_t::skipped(needs_directory_k));
    report(totals, "database.snapshot_with_threads", database_snapshot_with_threads, config);
    report(totals, "database.erase_missing", database_transaction_erase_missing, config);
    report(totals, "database.write_conflicting", database_transaction_write_conflicting, config);
    report(totals, "database.transaction_stamps", database_transaction_stamps, config);
    report(totals, "database.freed_first", database_freed_first, config);

    report(totals, "database.docs_flat", database_docs_flat, config);
    report(totals, "database.docs_nested_batch", database_docs_nested_batch, config);
    report(totals, "database.docs_modify", database_docs_modify, config);
    report(totals, "database.docs_merge_and_patch", database_docs_merge_and_patch, config);
    report(totals, "database.docs_table", database_docs_table, config);

    report(totals, "database.graph_write_vertices", database_graph_write_vertices, config);
    report(totals, "database.graph_write_edge", database_graph_write_edge, config);
    report(totals, "database.graph_triangle", database_graph_triangle, config);
    report(totals, "database.graph_triangle_batch", database_graph_triangle_batch, config);
    report(totals, "database.graph_transaction_watch", database_graph_transaction_watch, config);
    report(totals, "database.graph_random_fill", database_graph_random_fill, config);
    report(totals, "database.graph_conflicts", database_graph_conflicting_transactions, config);
    report(totals, "database.graph_layering_shapes", database_graph_layering_shapes, config);
    report(totals, "database.graph_remove_vertices", database_graph_remove_vertices, config);
    report(totals, "database.graph_remove_edges", database_graph_remove_edges_keep_vertices, config);
    report(totals, "database.graph_get_vertex_edges", database_graph_get_vertex_edges, config);
    report(totals, "database.graph_degrees", database_graph_degrees, config);
    report(totals, "database.graph_neighbors", database_graph_neighbors, config);

    report(totals, "database.pages_random_streams", database_pages_random_streams, config);
    report(totals, "database.pages_transactions", database_pages_transactions, config);
    report(totals, "database.pages_all_or_nothing", database_pages_all_or_nothing, config);

    report(totals, "executor.on_receivers", executor_on_receivers, config);
    report(totals, "executor.deferred_commit", executor_deferred_commit, config);
    report(totals, "executor.deferred_docs", executor_deferred_docs, config);
    report(totals, "executor.deferred_graph", executor_deferred_graph, config);
    report(totals, "executor.deferred_strings", executor_deferred_strings, config);
    report(totals, "executor.deferred_open", executor_deferred_open, config);
    report(totals, "executor.contended", executor_contended, config);

    return suite_exit_code(totals);
}
