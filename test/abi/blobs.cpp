/**
 *  @file test/abi/blobs.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Unit tests of the blobs modality through the C++ wrappers.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstdio>  // `stderr`

#include <algorithm>   // `std::ranges::find`
#include <array>       // `std::array`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`

#include <fmt/format.h>

#include <forkunion/atomics.hpp> // `fu::standard_atomic_ref`

#include "ustore/ustore.hpp"

#include "ustore/utilities/ram_device.hpp" // `caller_exclusivity_t`, `executor_t`

#include "abi/fixture.hpp" // `check_length`, `check_equalities`, `round_trip`, `triplet_t`
#include "harness.hpp"     // `suite_exit_code`, `expect`, `expect_eq`, `expect_status`, `spin_until`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::test::abi;
using namespace unum::ustore::utilities;
using namespace unum;

#pragma region Ranges

/** Checks that the postfix steps of a strided iterator move it and answer where it stood, as
 *  every iterator's do. */
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

/** Checks overwrites, cleared values keeping their keys, key streaming and removal
 *  on @p collection. */
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

/** Opens and clears a database, then round-trips values through a blobs collection. */
std::size_t database_open_clear_close(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    auto [collection, create_status] = database.create("blobs");
    failures += expect(create_status.succeeded());
    failures += check_binary_collection(collection);
    return failures;
}

/** Clearing the pairs of the whole database empties a blobs collection, and clearing the
 *  collections drops it. */
std::size_t database_clear_collection_by_clearing_database(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    auto [collection, create_status] = database.create("blobs");
    failures += expect(create_status.succeeded());
    triplet_t triplet;
    auto reference = collection[triplet.keys];
    failures += round_trip(reference, triplet);

    failures += expect(collection.keys().size() == 3u);
    failures += expect(collection.pairs().size() == 3u);

    failures += expect(database.clear(ustore_drop_pairs_k).succeeded());
    failures += check_length(reference, missing_length_k);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    failures += expect(!*database.contains("blobs"));
    return failures;
}

/** A description names the engine the suite opened, whether the head describes itself
 *  blocking or deferred. */
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

/** Clearing the pairs keeps a named collection while emptying it, and a deferred clear of the
 *  collections drops it. */
case_result_t database_clear_modes(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(open_status.succeeded() && executor_status.succeeded());
    if (failures != 0) return failures;
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

/** A compaction its progress callable cancels at the first report answers cancelled, blocking and
 *  deferred alike, and the data still reads. */
std::size_t database_compact(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [executor, executor_status] = executor_handle_t::for_ram({.threads = 2});
    failures += expect(open_status.succeeded() && executor_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    auto [collection, create_status] = database.create("blobs");
    failures += expect(create_status.succeeded());
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

/** Fills keys 1000 to 1100 and 900 down to 800, then overwrites every other key
 *  across both ranges. */
std::size_t database_overwrite_with_step(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    auto [collection, create_status] = database.create("blobs");
    failures += expect(create_status.succeeded());

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
        auto [kept_collection, kept_status] = database.create("kept");
        failures += expect(kept_status.succeeded());
        auto kept_reference = kept_collection[triplet.keys];
        failures += check_length(kept_reference, missing_length_k);
        failures += round_trip(kept_reference, triplet);
        failures += check_length(kept_reference, triplet_t::value_size_k);

        auto [emptied_collection, emptied_status] = database.create("emptied");
        failures += expect(emptied_status.succeeded());
        auto emptied_reference = emptied_collection[triplet.keys];
        failures += check_length(emptied_reference, missing_length_k);
        failures += round_trip(emptied_reference, triplet);
        failures += check_length(emptied_reference, triplet_t::value_size_k);
        failures += expect(emptied_collection.clear_values().succeeded());
        failures += check_length(emptied_reference, 0);
    }
    database.close();
    {
        auto [reopened, reopen_status] = open_database<database_t>(config);
        failures += expect(reopen_status.succeeded());
        if (failures != 0) return failures;
        database = std::move(reopened);

        auto [kept_collection, kept_status] = database.find("kept");
        failures += expect(kept_status.succeeded());
        auto kept_reference = kept_collection[triplet.keys];
        failures += check_equalities(kept_reference, triplet);
        failures += check_length(kept_reference, triplet_t::value_size_k);
        failures += expect(kept_collection.keys().size() == 3u);
        failures += expect(kept_collection.pairs().size() == 3u);

        failures += expect(database.contains("emptied"));
        failures += expect(*database.contains("emptied"));
        auto [emptied_collection, emptied_status] = database.find("emptied");
        failures += expect(emptied_status.succeeded());
        auto emptied_reference = emptied_collection[triplet.keys];
        failures += check_length(emptied_reference, 0);
        failures += expect(emptied_collection.keys().size() == 3u);
        failures += expect(emptied_collection.pairs().size() == 3u);
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

    // A missing collection can't be dropped, and the empty name carries none
    failures += expect(!*database.contains("unknown"));
    failures += expect(database.drop("unknown").failed());
    failures += expect(database.drop("").failed());

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
    failures += expect(!*database.contains(""));
    failures += expect(!*database.contains("col1"));
    failures += expect(!*database.contains("col2"));

    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    failures += expect(!*database.contains(""));
    return failures;
}

/** Threads racing to create one name leave exactly one collection behind, which
 *  the name addresses. */
case_result_t database_named_collections_race(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    constexpr std::size_t threads_count = 8;
    std::size_t created = 0;
    std::size_t refused = 0;
    auto task = [&](std::size_t) noexcept {
        auto [collection, create_status] = database.create("contested");
        if (create_status.succeeded()) fu::standard_atomic_ref<std::size_t> {created}.add(1, std::memory_order_relaxed);
        else if (create_status == status_code_t::already_exists_k)
            fu::standard_atomic_ref<std::size_t> {refused}.add(1, std::memory_order_relaxed);
    };
    auto maybe_executor = executor_t::make(threads_count, caller_exclusivity_t::caller_inclusive_k);
    failures += expect(maybe_executor);
    if (!maybe_executor) return failures;
    failures += expect(st::succeeded(maybe_executor->for_threads(task)));

    failures += expect_eq(created, 1u);
    failures += expect_eq(refused, threads_count - 1);
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

/** Lists every collection with its name and modality beside its identifier, an unnamed one with an
 *  empty name, where each kind of handle creates its own modality and finds no other. */
case_result_t database_named_collections_list(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    // Every kind of collection handle creates a collection of its own modality
    std::array<std::string_view, 3> const names {"col1", "col2", "col3"};
    std::array<ustore_modality_t, 3> const modalities {ustore_modality_blobs_k, ustore_modality_docs_k,
                                                       ustore_modality_graph_k};
    failures += expect(database.create<blobs_collection_t>(names[0]).has_value());
    failures += expect(database.create<docs_collection_t>(names[1]).has_value());
    failures += expect(database.create<graph_collection_t>(names[2]).has_value());
    ustore_outcome_t outcome {};
    ustore_collection_id_t unnamed {};
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

    // Every listed name resolves back to the identifier beside it, and the unnamed collection lists
    // an empty name
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

    // A typed lookup refuses a collection of another modality, and a typed creation makes one the
    // graph calls serve
    failures += expect(database.find<docs_collection_t>("col3") == status_code_t::invalid_argument_k);
    failures += expect(database.find<graph_collection_t>("col3").has_value());
    auto friends = database.find_or_create<graph_collection_t>("friends");
    failures += expect(friends && friends->write_edge(edge_t {key_from_integer(1), key_from_integer(2)}).succeeded());

    failures += expect(database.drop("col1").succeeded());
    failures += expect(!*database.contains("col1"));
    failures += expect(database.drop("").failed());
    return failures;
}

/** Every call resolves its collections, refusing identifiers nobody created and collections keyed
 *  the other way. */
case_result_t database_collections_resolve(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    // Nothing reaches an identifier the engine never handed out, the nil UUID among them, whatever
    // it keeps there itself
    for (ustore_collection_id_t const unknown :
         {ustore_collection_id_t {}, ustore_collection_id_t {uuid_from_integer(4242)},
          ustore_collection_id_t {largest_key_k.uuid}}) {
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
    auto [collection, create_status] = database.create("blobs");
    failures += expect(create_status.succeeded());
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
    auto [collection, create_status] = database.create("blobs");
    failures += expect(create_status.succeeded());

    constexpr std::size_t keys_count = 1000;
    for (std::size_t index = 0; index != keys_count; ++index)
        if (index % 10) failures += expect(collection[key_from_integer(index)].assign("value").succeeded());

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
    read.collections = {collection.member_ptr(), 0};
    read.keys = {keys.data(), sizeof(ustore_key_t)};
    read.presences = &found_presences;
    read.arena = arena;

    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(found_presences != nullptr);
    for (std::size_t index = 0; found_presences && index != keys_count; ++index)
        failures += expect(bits_view_t {found_presences}[index] == (index % 10 != 0));

    auto maybe_presences = collection[keys].present();
    failures += expect(maybe_presences);
    auto const& presences = *maybe_presences;
    for (std::size_t index = 0; index != keys_count; ++index)
        failures += expect(static_cast<bool>(presences[index]) == (index % 10 != 0));
    return failures;
}

/** One arena serves the reads of two collections in turn, each read rewinding it, while shorthand
 *  reads keep no arena anywhere and own the one-off arena they answer from. */
case_result_t database_arena_reuse(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
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

    // The shorthand answers outlive the collection handles that read them, since each owns
    // its own arena
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
    auto [collection, create_status] = database.create("blobs");
    failures += expect(create_status.succeeded());

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

/** Ordered batched scan over a blobs collection. */
std::size_t database_batch_scan(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    auto [collection, create_status] = database.create("blobs");
    failures += expect(create_status.succeeded());

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

/**
 *  @brief Readers cannot see the writes of an uncommitted transaction.
 *
 *  @see Read Committed: https://jepsen.io/consistency/models/read-committed
 */
case_result_t database_transaction_read_committed(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_transactions()) return case_result_t::skipped("the engine keeps no transactions", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    failures += expect(database.transaction());
    transaction_t transaction = *database.transaction();
    auto [collection, create_status] = database.create("blobs");
    failures += expect(create_status.succeeded());

    triplet_t triplet;
    auto transaction_reference = blobs_collection_t {transaction.receiver(), collection}[triplet.keys];
    failures += round_trip(transaction_reference, triplet);

    auto collection_reference = collection[triplet.keys];

    failures += check_length(collection_reference, missing_length_k);
    failures += expect(transaction.commit().has_value());
    failures += expect(transaction.reset().succeeded());

    failures += check_equalities(collection_reference, triplet);
    return failures;
}

/**
 *  @brief Reads through a snapshot miss writes made after it.
 *
 *  @see Snapshot Isolation: https://jepsen.io/consistency/models/snapshot-isolation
 */
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

    auto [collection, create_status] = database.create("blobs");
    failures += expect(create_status.succeeded());
    auto collection_reference = collection[triplet.keys];

    failures += check_length(collection_reference, missing_length_k);
    failures += round_trip(collection_reference, triplet);

    auto snapshot = *database.snapshot();
    auto snapshot_reference = blobs_read_collection_t {snapshot.receiver(), collection}[triplet.keys];
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
    auto reference = blobs_read_collection_t {snapshot.receiver(), collection}[same_values_triplet.keys];
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

    auto [collection, create_status] = database.create("blobs");
    failures += expect(create_status.succeeded());
    auto collection_reference = collection[triplet.keys];

    failures += check_length(collection_reference, missing_length_k);
    failures += round_trip(collection_reference, triplet);

    auto snapshot = *database.snapshot();
    auto snapshot_reference = blobs_read_collection_t {snapshot.receiver(), collection}[triplet.keys];
    failures += check_equalities(snapshot_reference, triplet);

    failures += round_trip(collection_reference, same_values_triplet);

    transaction_t transaction = *database.transaction();
    auto head_reference = blobs_collection_t {transaction.receiver(), collection}[triplet.keys];
    failures += check_equalities(head_reference, same_values_triplet);
    auto older_reference = blobs_read_collection_t {snapshot.receiver(), collection}[triplet.keys];
    failures += check_equalities(older_reference, triplet);

    snapshot = *database.snapshot();
    auto newer_reference = blobs_read_collection_t {snapshot.receiver(), collection}[triplet.keys];
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

    auto [collection, create_status] = database.create("blobs");
    failures += expect(create_status.succeeded());
    auto collection_reference = collection[triplet.keys];
    failures += round_trip(collection_reference, triplet);

    auto first_snapshot = *database.snapshot();
    failures += expect(first_snapshot.save(first_saved_directory.c_str()).succeeded());
    failures += check_equalities(collection_reference, triplet);

    config_t const first_config {config.options(), first_saved_directory.c_str()};
    auto [first_saved, first_saved_status] = open_database<database_t>(first_config);
    failures += expect(first_saved_status.succeeded());
    if (failures != 0) return failures;
    auto [first_collection, first_find_status] = first_saved.find("blobs");
    failures += expect(first_find_status.succeeded());
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
    auto [second_collection, second_find_status] = second_saved.find("blobs");
    failures += expect(second_find_status.succeeded());
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

/** A save hands its progress callable the bytes saved and the bytes in total, never
 *  decreasing, never past the total and ending at it, whether the snapshot saves itself
 *  blocking or deferred. */
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
    auto [collection, create_status] = database.create("blobs");
    failures += expect(create_status.succeeded());
    auto reference = collection[triplet.keys];
    failures += round_trip(reference, triplet);

    // Engines reporting per collection then report before the end, where the order of
    // the counts shows
    auto [named, named_status] = database.create("saved");
    failures += expect(named_status.succeeded());
    if (failures != 0) return failures;
    auto named_reference = named[triplet.keys];
    failures += round_trip(named_reference, triplet);

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

    auto [collection, create_status] = database.create("blobs");
    failures += expect(create_status.succeeded());
    auto reference = collection[triplet.keys];
    failures += round_trip(reference, triplet);

    auto snapshot = *database.snapshot();
    auto snapshot_reference = blobs_read_collection_t {snapshot.receiver(), collection}[triplet.keys];

    failures += round_trip(reference, same_values_triplet);

    bool snapshot_locked = false;
    bool is_reset = false;
    std::size_t thread_failures = 0;
    auto acquire = [&] {
        return spin_until(
            [&] { return !fu::standard_atomic_ref<bool> {snapshot_locked}.exchange(true, std::memory_order_acquire); });
    };

    auto task = [&](std::size_t thread) noexcept {
        std::size_t local_failures = 0;
        if (thread == 0) {
            while (true) {
                if (!acquire()) {
                    ++local_failures;
                    break;
                }
                if (is_reset) {
                    auto head_reference = blobs_read_collection_t {snapshot.receiver(), collection}[triplet.keys];
                    local_failures += check_equalities(head_reference, same_values_triplet);
                    fu::standard_atomic_ref<bool> {snapshot_locked}.store(false, std::memory_order_release);
                    break;
                }
                local_failures += check_equalities(snapshot_reference, triplet);
                fu::standard_atomic_ref<bool> {snapshot_locked}.store(false, std::memory_order_release);
            }
        }
        else if (acquire()) {
            snapshot = *database.snapshot();
            is_reset = true;
            fu::standard_atomic_ref<bool> {snapshot_locked}.store(false, std::memory_order_release);
        }
        else { ++local_failures; }
        fu::standard_atomic_ref<std::size_t> {thread_failures}.add(local_failures, std::memory_order_relaxed);
    };

    auto maybe_executor = executor_t::make(2, caller_exclusivity_t::caller_inclusive_k);
    failures += expect(maybe_executor);
    if (!maybe_executor) return failures;
    failures += expect(st::succeeded(maybe_executor->for_threads(task)));
    failures += thread_failures;
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
    auto [collection, create_status] = database.create("blobs");
    failures += expect(create_status.succeeded());
    transaction_t first_transaction = *database.transaction();
    transaction_t second_transaction = *database.transaction();
    blobs_collection_t first_inside {first_transaction.receiver(), collection};
    blobs_collection_t second_inside {second_transaction.receiver(), collection};

    failures += expect(second_inside.at(key_from_integer(7297309151944849401)).remove().succeeded());
    failures += expect(first_inside.at(key_from_integer(8640850744835793378)).remove().succeeded());
    failures += expect(first_transaction.commit().has_value());
    failures += expect(second_transaction.commit().has_value());

    failures += expect(collection.at(key_from_integer(8640850744835793378)).value() == value_view_t {});
    failures += expect(collection.at(key_from_integer(7297309151944849401)).value() == value_view_t {});
    failures += expect(collection.keys().size() == 0u);
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
    auto [collection, create_status] = database.create("blobs");
    failures += expect(create_status.succeeded());
    transaction_t first_transaction = *database.transaction();
    transaction_t second_transaction = *database.transaction();

    failures += expect(
        blobs_collection_t {second_transaction.receiver(), collection}.at(key_from_integer(6)).assign("a").succeeded());
    failures += expect(
        blobs_collection_t {first_transaction.receiver(), collection}.at(key_from_integer(6)).assign("b").succeeded());
    failures += expect(first_transaction.commit().has_value());
    failures += expect(second_transaction.commit().failed());
    return failures;
}

/** A stage answers the stamp the transaction reads at, its commit a position no lower, and a later
 *  snapshot reads at or above that position. */
case_result_t database_transaction_stamps(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_transactions()) return case_result_t::skipped("the engine keeps no transactions", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());

    failures += expect(database.transaction());
    transaction_t transaction = *database.transaction();
    auto [collection, create_status] = database.create("blobs");
    failures += expect(create_status.succeeded());

    triplet_t triplet;
    auto transaction_reference = blobs_collection_t {transaction.receiver(), collection}[triplet.keys];

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

/** A database destroyed before its transaction and snapshot stays open for both, each holding it
 *  until it goes, so the transaction still writes, commits and resets, and the snapshot still reads
 *  what it pinned. */
case_result_t database_freed_first(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_transactions() && !database.supports_snapshots())
        return case_result_t::skipped("the engine keeps neither transactions nor snapshots", failures);
    failures += expect(database.clear(ustore_drop_collection_k).succeeded());
    ustore_key_t const key = key_from_integer(91);
    auto [head, create_status] = database.create("blobs");
    failures += expect(create_status.succeeded());
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
        blobs_collection_t inside {transaction->receiver(), head};
        failures += expect(inside[key].assign("after").succeeded());
        failures += expect(transaction->commit().has_value());
        failures += expect(transaction->reset().succeeded());
        failures += expect(inside[key].value() == "after");
    }
    if (snapshot) {
        blobs_read_collection_t pinned {snapshot->receiver(), head};
        failures += expect(pinned[key].value() == "before");
    }
    return failures;
}

#pragma endregion Binary Modality

int main(int argc, char** argv) {
    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    auto const maybe_directory = directory_t::make(options.directory);
    if (!maybe_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", options.directory);
        return 1;
    }
    char const* const directory = maybe_directory->path();
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
    return suite_exit_code(totals);
}
