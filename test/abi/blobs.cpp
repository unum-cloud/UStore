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

#include <algorithm>   // `std::ranges::find`, `std::equal`
#include <array>       // `std::array`
#include <span>        // `std::span`
#include <string>      // `std::string`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`

#include <fmt/format.h>

#include <forkunion/atomics.hpp>   // `fu::standard_atomic_ref`
#include <forkunion/forkunion.hpp> // `fu::topology_t`, `fu::pool_t`

#include "ustore/ustore.hpp"

#include "abi/fixture.hpp" // `check_length`, `check_equalities`, `round_trip`, `triplet_t`
#include "harness.hpp"     // `run_test_case`, `expect_true`, `expect_equal`, `expect_status`, `spin_until`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::test::abi;
using namespace unum;

#pragma region Ranges

/** Checks that the postfix steps of a strided iterator move it and answer where it stood, as
 *  every iterator's do. */
void test_strided_iterator_postfix_steps() {
    std::array<ustore_key_t, 2> const keys {key_from_integer(1), key_from_integer(2)};
    strided_iterator<ustore_key_t const> iterator = strided(keys).begin();
    expect_true(*iterator++ == keys[0]);
    expect_true(*iterator == keys[1]);
    expect_true(*iterator-- == keys[1]);
    expect_true(*iterator == keys[0]);
}

#pragma endregion Ranges

#pragma region Binary Modality

/** Checks overwrites, cleared values keeping their keys, key streaming and removal
 *  on @p collection. */
void check_binary_collection(blobs_collection_t& collection) {
    triplet_t triplet;
    auto reference = collection[triplet.keys];
    round_trip(reference, triplet);

    // Overwrite with values of the same size
    for (char& value : triplet.values) value += 7;
    round_trip(reference, triplet);

    // Empty values keep their keys
    expect_true(!reference.clear_values().failed());
    check_length(reference, 0);

    keys_range_t present_keys = collection.keys();
    auto present_iterator = present_keys.begin();
    for (ustore_key_t const expected_key : triplet.keys) {
        if (!expect_true(present_iterator != present_keys.end())) break;
        expect_equal(*present_iterator, expected_key);
        ++present_iterator;
    }
    expect_true(present_iterator == present_keys.end());
    expect_true(!present_keys.status().failed());

    expect_true(!reference.remove().failed());
    check_length(reference, missing_length_k);
}

/** Opens and clears a database, then round-trips values through a blobs collection. */
void test_database_open_clear_close(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    auto [collection, create_status] = database.create("blobs");
    expect_true(!create_status.failed());
    check_binary_collection(collection);
}

/** Clearing the pairs of the whole database empties a blobs collection, and clearing the
 *  collections drops it. */
void test_database_clear_collection_by_clearing_database(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    auto [collection, create_status] = database.create("blobs");
    expect_true(!create_status.failed());
    triplet_t triplet;
    auto reference = collection[triplet.keys];
    round_trip(reference, triplet);

    expect_true(answers(collection.keys().size(), 3u));
    expect_true(answers(collection.pairs().size(), 3u));

    expect_true(!database.clear(ustore_drop_pairs_k).failed());
    check_length(reference, missing_length_k);
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    expect_true(answers(database.contains("blobs"), false));
}

/** A description names the engine the suite opened, whether the head describes itself
 *  blocking or deferred. */
void test_database_describe(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [executor, executor_status] = spawn_executor_handle();
    if (!expect_true(!open_status.failed() && !executor_status.failed())) return;

    auto [description, describe_status] = database.describe();
    auto [deferred_description, deferred_status] = database.deferred(executor).describe().wait();
    expect_true(!describe_status.failed() && !deferred_status.failed());
    expect_equal(std::string_view {ustore_engine_name(description.engine)}, config.engine());
    expect_equal(std::string_view {ustore_engine_name(deferred_description.engine)}, config.engine());
}

/** Clearing the pairs keeps a named collection while emptying it, and a deferred clear of the
 *  collections drops it. */
case_result_t test_database_clear_modes(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [executor, executor_status] = spawn_executor_handle();
    if (!expect_true(!open_status.failed() && !executor_status.failed())) return {};
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    auto [kept, create_status] = database.create("kept");
    if (!expect_true(!create_status.failed())) return {};
    expect_true(!kept[key_from_integer(5)].assign("value").failed());

    expect_true(!database.clear(ustore_drop_pairs_k).failed());
    expect_true(answers(database.contains("kept"), true));
    expect_true(answers(kept.keys().size(), 0u));

    expect_true(!database.deferred(executor).clear(ustore_drop_collection_k).wait().failed());
    expect_true(answers(database.contains("kept"), false));
    return {};
}

/** A compaction its progress callable cancels at the first report answers cancelled, blocking and
 *  deferred alike, and the data still reads. */
void test_database_compact(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [executor, executor_status] = spawn_executor_handle();
    if (!expect_true(!open_status.failed() && !executor_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    auto [collection, create_status] = database.create("blobs");
    expect_true(!create_status.failed());
    expect_true(!collection[key_from_integer(11)].assign("compacted").failed());

    std::size_t reports = 0;
    auto cancelling = [&](ustore_size_t, ustore_size_t) noexcept {
        ++reports;
        return ustore_cancel_k;
    };
    expect_status(database.compact(cancelling), status_code_t::cancelled_k);
    expect_status(database.deferred(executor).compact(cancelling).wait(), status_code_t::cancelled_k);
    expect_equal(reports, 2u);
    expect_true(!database.compact().failed());
    auto [compacted, compacted_status] = collection[key_from_integer(11)].value();
    expect_true(!compacted_status.failed() && compacted == "compacted");
}

/** Fills keys 1000 to 1100 and 900 down to 800, then overwrites every other key
 *  across both ranges. */
void test_database_overwrite_with_step(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    auto [collection, create_status] = database.create("blobs");
    expect_true(!create_status.failed());

    for (std::uint64_t key = 1000; key != 1100; ++key)
        expect_true(!collection[key_from_integer(key)].assign("some").failed());
    for (std::uint64_t key = 1000; key != 1100; ++key) {
        auto [value, value_status] = collection[key_from_integer(key)].value();
        expect_true(!value_status.failed() && value == "some");
    }
    expect_true(answers(collection.keys().size(), 100u));
    expect_true(answers(collection.pairs().size(), 100u));

    for (std::uint64_t key = 900; key != 800; --key)
        expect_true(!collection[key_from_integer(key)].assign("other").failed());
    for (std::uint64_t key = 900; key != 800; --key) {
        auto [value, value_status] = collection[key_from_integer(key)].value();
        expect_true(!value_status.failed() && value == "other");
    }
    expect_true(answers(collection.keys().size(), 200u));
    expect_true(answers(collection.pairs().size(), 200u));

    for (std::uint64_t key = 800; key != 1100; key += 2)
        expect_true(!collection[key_from_integer(key)].assign("third").failed());
    for (std::uint64_t key = 800; key != 1100; key += 2) {
        auto [value, value_status] = collection[key_from_integer(key)].value();
        expect_true(!value_status.failed() && value == "third");
    }
    expect_true(answers(collection.keys().size(), 250u));
    expect_true(answers(collection.pairs().size(), 250u));
}

/** Populates the database, closes and reopens it, and checks the contents survived. */
void test_database_persistency(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    triplet_t triplet;
    {
        auto [kept_collection, kept_status] = database.create("kept");
        expect_true(!kept_status.failed());
        auto kept_reference = kept_collection[triplet.keys];
        check_length(kept_reference, missing_length_k);
        round_trip(kept_reference, triplet);
        check_length(kept_reference, triplet_t::value_size_k);

        auto [emptied_collection, emptied_status] = database.create("emptied");
        expect_true(!emptied_status.failed());
        auto emptied_reference = emptied_collection[triplet.keys];
        check_length(emptied_reference, missing_length_k);
        round_trip(emptied_reference, triplet);
        check_length(emptied_reference, triplet_t::value_size_k);
        expect_true(!emptied_collection.clear_values().failed());
        check_length(emptied_reference, 0);
    }
    // A close lands nothing, so the flush is what the reopen reads
    expect_true(!database.flush().failed());
    database.close();
    {
        auto [reopened, reopen_status] = open_database<database_t>(config);
        if (!expect_true(!reopen_status.failed())) return;
        database = std::move(reopened);

        auto [kept_collection, kept_status] = database.find("kept");
        expect_true(!kept_status.failed());
        auto kept_reference = kept_collection[triplet.keys];
        check_equalities(kept_reference, triplet);
        check_length(kept_reference, triplet_t::value_size_k);
        expect_true(answers(kept_collection.keys().size(), 3u));
        expect_true(answers(kept_collection.pairs().size(), 3u));

        expect_true(!database.contains("emptied").status.failed());
        expect_true(answers(database.contains("emptied"), true));
        auto [emptied_collection, emptied_status] = database.find("emptied");
        expect_true(!emptied_status.failed());
        auto emptied_reference = emptied_collection[triplet.keys];
        check_length(emptied_reference, 0);
        expect_true(answers(emptied_collection.keys().size(), 3u));
        expect_true(answers(emptied_collection.pairs().size(), 3u));
    }
}

/** Creates, looks up, drops and clears collections under unique names. */
void test_database_named_collections(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    // A missing collection can't be dropped, and the empty name holds none
    expect_true(answers(database.contains("unknown"), false));
    expect_true(database.drop("unknown").failed());
    expect_true(database.drop("").failed());

    expect_true(!database.find_or_create("col1").status.failed());
    expect_true(!database.find_or_create("col2").status.failed());

    expect_true(database.create("col1").status.failed());
    auto [first_collection, first_collection_status] = database.find_or_create("col1");
    if (!expect_true(!first_collection_status.failed())) return;
    expect_true(database.create("col2").status.failed());
    auto [second_collection, second_collection_status] = database.find_or_create("col2");
    if (!expect_true(!second_collection_status.failed())) return;

    check_binary_collection(first_collection);
    check_binary_collection(second_collection);

    expect_true(!database.drop("col1").failed());
    expect_true(!database.drop("col2").failed());
    expect_true(answers(database.contains(""), false));
    expect_true(answers(database.contains("col1"), false));
    expect_true(answers(database.contains("col2"), false));

    expect_true(!database.clear(ustore_drop_collection_k).failed());
    expect_true(answers(database.contains(""), false));
}

/** Threads racing to create one name leave exactly one collection behind, which
 *  the name addresses. */
case_result_t test_database_named_collections_race(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    constexpr std::size_t threads_count = 8;
    std::size_t created = 0;
    std::size_t refused = 0;
    auto task = [&](std::size_t) noexcept {
        auto [collection, create_status] = database.create("contested");
        if (!create_status.failed())
            fu::standard_atomic_ref<std::size_t> {created}.store_add(1, std::memory_order_relaxed);
        else if (create_status == status_code_t::already_exists_k)
            fu::standard_atomic_ref<std::size_t> {refused}.store_add(1, std::memory_order_relaxed);
    };
    fu::topology_t topology;
    fu::pool_t executor;
    fu::status_t const executor_status = fu::failed(topology.harvest())
                                             ? fu::status_t::topology_unavailable_k
                                             : executor.spawn(topology, threads_count, fu::caller_inclusive_k);
    expect_true(!fu::failed(executor_status));
    if (fu::failed(executor_status)) return {};
    expect_true(!fu::failed(executor.for_threads(task)));

    expect_equal(created, 1u);
    expect_equal(refused, threads_count - 1);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed())) return {};
    auto [collections, list_status] = database.collections(arena);
    auto [found, find_status] = database.find("contested");
    expect_true(!list_status.failed() && !find_status.failed());
    expect_equal(collections.size(), 1u);
    expect_true(collections.size() == 1 && found.handle() == collections[0]);
    expect_true(!database.drop("contested").failed());
    return {};
}

/** Lists every collection with its name and modality beside its identifier, an unnamed one with an
 *  empty name, where each kind of handle creates its own modality and finds no other. */
case_result_t test_database_named_collections_list(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    // Every kind of collection handle creates a collection of its own modality
    std::array<std::string_view, 3> const names {"col1", "col2", "col3"};
    std::array<ustore_modality_t, 3> const modalities {ustore_modality_blobs_k, ustore_modality_docs_k,
                                                       ustore_modality_graph_k};
    expect_true(!database.create<blobs_collection_t>(names[0]).status.failed());
    expect_true(!database.create<docs_collection_t>(names[1]).status.failed());
    expect_true(!database.create<graph_collection_t>(names[2]).status.failed());
    ustore_outcome_t outcome {};
    ustore_collection_id_t unnamed {};
    ustore_collection_create_t create {};
    create.database = database.handle();
    create.outcome = &outcome;
    create.modality = ustore_modality_strings_k;
    create.collection = &unnamed;
    ustore_collection_create(&create);
    expect_success(outcome);

    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed())) return {};
    ustore_size_t written = 0;
    ustore_collection_id_t const* ids = nullptr;
    ustore_modality_t const* listed_modalities = nullptr;
    ustore_strings_view_t listed_names {};
    ustore_collection_list_t list {};
    list.database = database.handle();
    list.outcome = &outcome;
    list.collections = &ids;
    list.modalities = &listed_modalities;
    list.names = &listed_names;
    list.written = &written;
    list.arena = arena.handle();
    ustore_collection_list(&list);
    expect_success(outcome);
    expect_equal(written, 4u);
    if (written != 4) return {};

    // Every listed name resolves back to the identifier beside it, and the unnamed collection lists
    // an empty name
    for (std::size_t index = 0; index != written; ++index) {
        std::string_view const name = util::chars_of(util::tape_string(listed_names, index));
        if (name.empty()) {
            expect_true(ids[index] == unnamed && listed_modalities[index] == ustore_modality_strings_k);
            continue;
        }
        auto const named = std::ranges::find(names, name);
        expect_true(named != names.end());
        if (named == names.end()) continue;
        expect_true(listed_modalities[index] == modalities[named - names.begin()]);
        auto const [found, find_status] = collection_named(database.handle(), name);
        expect_true(!find_status.failed() && found == ids[index]);
    }

    // A typed lookup refuses a collection of another modality, and a typed creation makes one the
    // graph calls serve
    expect_true(database.find<docs_collection_t>("col3").status == status_code_t::invalid_argument_k);
    expect_true(!database.find<graph_collection_t>("col3").status.failed());
    auto [friends, friends_status] = database.find_or_create<graph_collection_t>("friends");
    expect_true(!friends_status.failed() &&
                !friends.write_edge(edge_t {key_from_integer(1), key_from_integer(2)}).failed());

    expect_true(!database.drop("col1").failed());
    expect_true(answers(database.contains("col1"), false));
    expect_true(database.drop("").failed());
    return {};
}

/** Every call resolves its collections, refusing identifiers nobody created and collections keyed
 *  the other way. */
case_result_t test_database_collections_resolve(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    // Nothing reaches an identifier the engine never handed out, the nil UUID among them, whatever
    // it keeps there itself
    for (ustore_collection_id_t const unknown :
         {ustore_collection_id_t {}, ustore_collection_id_t {uuid_from_integer(4242)},
          ustore_collection_id_t {largest_key_k.uuid}}) {
        blobs_collection_t keyed {database.receiver(), unknown};
        expect_true(keyed[key_from_integer(1)].value().status == status_code_t::not_found_k);
        expect_true(keyed[key_from_integer(1)].assign("intruder") == status_code_t::not_found_k);
        expect_true(keyed.clear() == status_code_t::not_found_k);
        strings_collection_t stringed {database.receiver(), unknown};
        expect_true(stringed.assign("intruder", key_from_integer(1)) == status_code_t::not_found_k);
    }

    // A modality fixes how a collection is keyed, and calls keyed the other way are refused
    auto [words_collection, words_status] = database.create<strings_collection_t>("words");
    auto [numbers_collection, numbers_status] = database.create("numbers");
    expect_true(!words_status.failed() && !numbers_status.failed());
    if (words_status.failed() || numbers_status.failed()) return {};
    ustore_collection_id_t const words = words_collection.handle();
    blobs_collection_t keyed_words {database.receiver(), words};
    expect_true(keyed_words[key_from_integer(1)].assign("number") == status_code_t::invalid_argument_k);
    strings_collection_t stringed_numbers {database.receiver(), numbers_collection.handle()};
    expect_true(stringed_numbers.assign("word", key_from_integer(1)) == status_code_t::invalid_argument_k);
    expect_true(!words_collection.assign("word", key_from_integer(1)).failed());

    // A name is never a collection, and dropping a collection by identifier takes its name too
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed())) return {};
    auto [listed, list_status] = database.collections(arena);
    expect_true(!list_status.failed());
    expect_equal(listed.size(), 2u);
    ustore_outcome_t outcome {};
    ustore_collection_drop_t drop {};
    drop.database = database.handle();
    drop.outcome = &outcome;
    drop.collection = words;
    drop.mode = ustore_drop_collection_k;
    ustore_collection_drop(&drop);
    expect_success(outcome);
    expect_true(answers(database.contains("words"), false));
    expect_true(!database.drop("numbers").failed());
    return {};
}

/** Clearing values keeps the keys and empties their binary strings. */
void test_database_clear_values(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    triplet_t triplet;
    auto [collection, create_status] = database.create("blobs");
    expect_true(!create_status.failed());
    auto reference = collection[triplet.keys];

    check_length(reference, missing_length_k);
    round_trip(reference, triplet);
    check_length(reference, triplet_t::value_size_k);

    expect_true(!collection.clear_values().failed());
    check_length(reference, 0);
    expect_true(!collection.clear().failed());
    check_length(reference, missing_length_k);
}

/** Reads presences through the C ABI and the C++ wrappers. */
void test_database_presences(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    auto [collection, create_status] = database.create("blobs");
    expect_true(!create_status.failed());

    constexpr std::size_t keys_count = 1000;
    for (std::size_t index = 0; index != keys_count; ++index)
        if (index % 10) expect_true(!collection[key_from_integer(index)].assign("value").failed());

    std::array<ustore_key_t, keys_count> keys;
    for (std::size_t index = 0; index != keys_count; ++index) keys[index] = key_from_integer(index);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed())) return;
    ustore_octet_t const* found_presences = nullptr;
    ustore_outcome_t outcome {};
    ustore_collection_id_t const collection_id = collection.handle();
    ustore_blobs_read_t read {};
    read.database = database.handle();
    read.outcome = &outcome;
    read.count = keys_count;
    read.collections = {&collection_id, 0};
    read.keys = {keys.data(), sizeof(ustore_key_t)};
    read.presences = &found_presences;
    read.arena = arena.handle();

    ustore_blobs_read(&read);
    expect_success(outcome);
    expect_true(found_presences != nullptr);
    for (std::size_t index = 0; found_presences && index != keys_count; ++index)
        expect_true(bits_view_t {found_presences}[index] == (index % 10 != 0));

    auto [presences, presences_status] = collection[keys].present();
    expect_true(!presences_status.failed());
    for (std::size_t index = 0; index != keys_count; ++index)
        expect_true(static_cast<bool>(presences[index]) == (index % 10 != 0));
}

/** One batched read across two collections, missing keys and repeats, answers the very tape,
 *  lengths and presences that reading each key alone answers, before and after a compaction moves
 *  the values out of memory. */
void test_database_batched_read_matches_single(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    auto [first, first_status] = database.create("first");
    auto [second, second_status] = database.create("second");
    if (!expect_true(!first_status.failed() && !second_status.failed())) return;

    // Every seventh key stays missing, every eleventh holds an empty value, the rest grow with it.
    constexpr std::size_t keys_count = 3000;
    for (std::size_t index = 0; index != keys_count; ++index) {
        if (index % 7 == 0) continue;
        std::string const value = index % 11 == 0 ? std::string {}
                                                  : std::string(1 + index % 600, char('a' + index % 26));
        auto& collection = index % 2 ? first : second;
        expect_true(!collection[key_from_integer(index)]
                         .assign(value_view_t {reinterpret_cast<ustore_byte_t const*>(value.data()), value.size()})
                         .failed());
    }
    std::array<ustore_key_t, keys_count + 64> keys;
    std::array<ustore_collection_id_t, keys_count + 64> collections;
    for (std::size_t index = 0; index != keys.size(); ++index) {
        std::size_t const key = (index * 7919) % keys_count;
        keys[index] = key_from_integer(key);
        collections[index] = key % 2 ? first.handle() : second.handle();
    }
    auto [arena, arena_status] = arena_t::make();
    auto [single_arena, single_arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed() && !single_arena_status.failed())) return;

    auto compare = [&]() {
        ustore_outcome_t outcome {};
        ustore_octet_t const* presences = nullptr;
        ustore_length_t const* lengths = nullptr;
        ustore_strings_view_t tape {};
        ustore_blobs_read_t read {.database = database.handle(),
                                  .outcome = &outcome,
                                  .count = keys.size(),
                                  .collections = {collections.data(), sizeof(ustore_collection_id_t)},
                                  .keys = {keys.data(), sizeof(ustore_key_t)},
                                  .presences = &presences,
                                  .lengths = &lengths,
                                  .values = &tape,
                                  .arena = arena.handle()};
        ustore_blobs_read(&read);
        if (!expect_success(outcome)) return;
        std::size_t mismatches = 0;
        for (std::size_t index = 0; index != keys.size(); ++index) {
            ustore_octet_t const* single_presences = nullptr;
            ustore_length_t const* single_lengths = nullptr;
            ustore_strings_view_t single_tape {};
            ustore_blobs_read_t alone {.database = database.handle(),
                                       .outcome = &outcome,
                                       .count = 1,
                                       .collections = {&collections[index], 0},
                                       .keys = {&keys[index], 0},
                                       .presences = &single_presences,
                                       .lengths = &single_lengths,
                                       .values = &single_tape,
                                       .arena = single_arena.handle()};
            ustore_blobs_read(&alone);
            if (outcome.status != ustore_status_success_k) {
                ++mismatches;
                continue;
            }
            std::string_view const batched = util::chars_of(util::tape_string(tape, index));
            std::string_view const single = util::chars_of(util::tape_string(single_tape, 0));
            mismatches += batched != single || lengths[index] != single_lengths[0] ||
                          bits_view_t {presences}[index] != bits_view_t {single_presences}[0];
        }
        expect_equal(mismatches, std::size_t {0});
    };
    compare();
    expect_true(!database.compact().failed());
    compare();
}

/** One arena serves the reads of two collections in turn, each read rewinding it, while shorthand
 *  reads keep no arena anywhere and own the one-off arena they answer from. */
case_result_t test_database_arena_reuse(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    auto [first, first_status] = database.create("first");
    auto [second, second_status] = database.create("second");
    if (!expect_true(!first_status.failed() && !second_status.failed())) return {};
    std::array<ustore_key_t, 2> const keys {key_from_integer(1), key_from_integer(2)};
    std::array<value_view_t, 2> const first_values {"alpha", "beta"};
    std::array<value_view_t, 2> const second_values {"gamma", "a much longer delta"};
    expect_true(!first[keys].assign(first_values).failed());
    expect_true(!second[keys].assign(second_values).failed());

    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed())) return {};
    for (std::size_t round = 0; round != 2; ++round) {
        auto [from_first, first_read_status] = first.read(keys, arena);
        expect_true(!first_read_status.failed());
        for (std::size_t index = 0; !first_read_status.failed() && index != keys.size(); ++index)
            expect_true(from_first[index] == first_values[index]);

        auto [from_second, second_read_status] = second.read(keys, arena);
        expect_true(!second_read_status.failed());
        for (std::size_t index = 0; !second_read_status.failed() && index != keys.size(); ++index)
            expect_true(from_second[index] == second_values[index]);
    }

    // The shorthand answers outlive the collection handles that read them, since each owns
    // its own arena
    auto [kept, kept_status] = first[keys[0]].value();
    auto [kept_batch, batch_status] = second[keys].value();
    first = blobs_collection_t {};
    second = blobs_collection_t {};
    expect_true(!kept_status.failed() && kept == first_values[0]);
    expect_true(!batch_status.failed() && kept_batch.size() == 2 && kept_batch[1] == second_values[1]);
    expect_true(!database.drop("first").failed() && !database.drop("second").failed());
    return {};
}

/** A key stream visits every written key in order. */
void test_database_scan(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    auto [collection, create_status] = database.create("blobs");
    expect_true(!create_status.failed());

    constexpr std::size_t keys_count = 1000;
    std::array<ustore_key_t, keys_count> keys;
    for (std::size_t index = 0; index != keys_count; ++index) keys[index] = key_from_integer(index);
    auto reference = collection[keys];
    value_view_t const value("value");
    expect_true(!reference.assign(value).failed());
    auto [stream, stream_status] = keys_stream_t::make(database.receiver(), collection.handle(), smallest_key_k, 256);
    expect_true(!stream_status.failed());
    std::size_t count = 0;
    while (!stream.is_end()) {
        expect_equal(stream.key(), key_from_integer(count++));
        ++stream;
    }
    expect_equal(count, keys_count);
}

/** Ordered batched scan over a blobs collection. */
void test_database_batch_scan(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    auto [collection, create_status] = database.create("blobs");
    expect_true(!create_status.failed());

    std::array<ustore_key_t, 512> keys;
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = key_from_integer(index);
    auto reference = collection[keys];
    value_view_t const value("value");
    expect_true(!reference.assign(value).failed());
    auto [stream, stream_status] = keys_stream_t::make(database.receiver(), collection.handle(), smallest_key_k, 256);
    expect_true(!stream_status.failed());
    auto batch = stream.keys_batch();
    expect_equal(batch.size(), 256u);
    expect_true(!stream.is_end());
    for (std::size_t index = 0; index != 256; ++index) expect_equal(batch[index], key_from_integer(index));

    expect_true(!stream.seek_to_next_batch().failed());
    batch = stream.keys_batch();
    expect_equal(batch.size(), 256u);
    expect_true(!stream.is_end());
    for (std::size_t index = 0; index != 256; ++index) expect_equal(batch[index], key_from_integer(index + 256));

    expect_true(!stream.seek_to_next_batch().failed());
    batch = stream.keys_batch();
    expect_equal(batch.size(), 0u);
    expect_true(stream.is_end());
}

/**
 *  @brief Readers cannot see the writes of an uncommitted transaction.
 *
 *  @see Read Committed: https://jepsen.io/consistency/models/read-committed
 */
case_result_t test_database_transaction_read_committed(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    if (!probed(database.supports_transactions())) return case_result_t::skipped(needs_transactions_k);
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    expect_true(!database.transaction().status.failed());
    auto [transaction, transaction_status] = database.transaction();
    if (!expect_true(!transaction_status.failed())) return {};
    auto [collection, create_status] = database.create("blobs");
    expect_true(!create_status.failed());

    triplet_t triplet;
    auto transaction_reference = blobs_collection_t {transaction.receiver(), collection.handle()}[triplet.keys];
    round_trip(transaction_reference, triplet);

    auto collection_reference = collection[triplet.keys];

    check_length(collection_reference, missing_length_k);
    expect_true(!transaction.commit().status.failed());
    expect_true(!transaction.reset().status.failed());

    check_equalities(collection_reference, triplet);
    return {};
}

/**
 *  @brief Reads through a snapshot miss writes made after it.
 *
 *  @see Snapshot Isolation: https://jepsen.io/consistency/models/snapshot-isolation
 */
case_result_t test_database_transaction_snapshot_isolation(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    if (!probed(database.supports_snapshots())) return case_result_t::skipped(needs_snapshots_k);
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    triplet_t triplet;
    triplet_t same_values_triplet;
    same_values_triplet.values = {'D', 'D', 'D'};

    auto [collection, create_status] = database.create("blobs");
    expect_true(!create_status.failed());
    auto collection_reference = collection[triplet.keys];

    check_length(collection_reference, missing_length_k);
    round_trip(collection_reference, triplet);

    auto [snapshot, snapshot_status] = database.snapshot();
    if (!expect_true(!snapshot_status.failed())) return {};
    auto snapshot_reference = blobs_read_collection_t {snapshot.receiver(), collection.handle()}[triplet.keys];
    check_equalities(snapshot_reference, triplet);
    round_trip(collection_reference, same_values_triplet);

    auto [retrieved, retrieve_status] = snapshot_reference.value();
    expect_true(!retrieve_status.failed());
    if (retrieve_status.failed()) return {};
    auto iterator = retrieved.begin();
    for (value_view_t const newer_view : same_values_triplet.views()) {
        value_view_t const retrieved_view = *iterator;
        expect_equal(retrieved_view.size(), newer_view.size());
        expect_true(retrieved_view != newer_view);
        ++iterator;
    }

    auto [renewed, renewed_status] = database.snapshot();
    if (!expect_true(!renewed_status.failed())) return {};
    snapshot = std::move(renewed);
    auto reference = blobs_read_collection_t {snapshot.receiver(), collection.handle()}[same_values_triplet.keys];
    check_equalities(reference, same_values_triplet);
    return {};
}

/** A transaction reads the head while a snapshot reads its own view. */
case_result_t test_database_transaction_with_snapshot(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    if (!probed(database.supports_snapshots())) return case_result_t::skipped(needs_snapshots_k);
    if (!probed(database.supports_transactions())) return case_result_t::skipped(needs_transactions_k);
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    triplet_t triplet;
    triplet_t same_values_triplet;
    same_values_triplet.values = {'D', 'D', 'D'};

    auto [collection, create_status] = database.create("blobs");
    expect_true(!create_status.failed());
    auto collection_reference = collection[triplet.keys];

    check_length(collection_reference, missing_length_k);
    round_trip(collection_reference, triplet);

    auto [snapshot, snapshot_status] = database.snapshot();
    if (!expect_true(!snapshot_status.failed())) return {};
    auto snapshot_reference = blobs_read_collection_t {snapshot.receiver(), collection.handle()}[triplet.keys];
    check_equalities(snapshot_reference, triplet);

    round_trip(collection_reference, same_values_triplet);

    auto [transaction, transaction_status] = database.transaction();
    if (!expect_true(!transaction_status.failed())) return {};
    auto head_reference = blobs_collection_t {transaction.receiver(), collection.handle()}[triplet.keys];
    check_equalities(head_reference, same_values_triplet);
    auto older_reference = blobs_read_collection_t {snapshot.receiver(), collection.handle()}[triplet.keys];
    check_equalities(older_reference, triplet);

    auto [renewed, renewed_status] = database.snapshot();
    if (!expect_true(!renewed_status.failed())) return {};
    snapshot = std::move(renewed);
    auto newer_reference = blobs_read_collection_t {snapshot.receiver(), collection.handle()}[triplet.keys];
    check_equalities(newer_reference, same_values_triplet);
    return {};
}

/** Saves snapshots as databases, then opens and modifies them independently of the original. */
case_result_t test_database_save_snapshot(config_t const& config) {
    std::string const original_directory = fmt::format("{}/{}", config.directory(), "original/");
    std::string const first_saved_directory = fmt::format("{}/{}", config.directory(), "saved1/");
    std::string const second_saved_directory = fmt::format("{}/{}", config.directory(), "saved2/");
    expect_true(scratch_directory_t::recreate(original_directory.c_str()));
    expect_true(scratch_directory_t::recreate(first_saved_directory.c_str()));
    expect_true(scratch_directory_t::recreate(second_saved_directory.c_str()));

    config_t const original_config {config.options(), original_directory.c_str()};
    auto [database, open_status] = open_database<database_t>(original_config);
    if (!expect_true(!open_status.failed())) return {};
    if (!probed(database.supports_snapshots())) return case_result_t::skipped(needs_snapshots_k);
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    triplet_t triplet;
    triplet_t same_values_triplet;
    same_values_triplet.values = {'D', 'D', 'D'};

    auto [collection, create_status] = database.create("blobs");
    expect_true(!create_status.failed());
    auto collection_reference = collection[triplet.keys];
    round_trip(collection_reference, triplet);

    auto [first_snapshot, first_snapshot_status] = database.snapshot();
    if (!expect_true(!first_snapshot_status.failed())) return {};
    expect_true(!first_snapshot.save(save_target(config, first_saved_directory).c_str()).failed());
    check_equalities(collection_reference, triplet);

    config_t const first_config {config.options(), first_saved_directory.c_str()};
    auto [first_saved, first_saved_status] = open_database<database_t>(first_config);
    if (!expect_true(!first_saved_status.failed())) return {};
    auto [first_collection, first_find_status] = first_saved.find("blobs");
    expect_true(!first_find_status.failed());
    auto first_reference = first_collection[triplet.keys];
    check_equalities(first_reference, triplet);

    round_trip(collection_reference, same_values_triplet);

    auto [second_snapshot, second_snapshot_status] = database.snapshot();
    if (!expect_true(!second_snapshot_status.failed())) return {};
    expect_true(!second_snapshot.save(save_target(config, second_saved_directory).c_str()).failed());
    check_equalities(collection_reference, same_values_triplet);

    config_t const second_config {config.options(), second_saved_directory.c_str()};
    auto [second_saved, second_saved_status] = open_database<database_t>(second_config);
    if (!expect_true(!second_saved_status.failed())) return {};
    auto [second_collection, second_find_status] = second_saved.find("blobs");
    expect_true(!second_find_status.failed());
    auto second_reference = second_collection[same_values_triplet.keys];
    check_equalities(second_reference, same_values_triplet);

    check_equalities(first_reference, triplet);
    check_equalities(second_reference, same_values_triplet);
    round_trip(first_reference, same_values_triplet);
    round_trip(second_reference, triplet);
    check_equalities(first_reference, same_values_triplet);
    check_equalities(second_reference, triplet);
    return {};
}

/** A save hands its progress callable the bytes saved and the bytes in total, never
 *  decreasing, never past the total and ending at it, whether the snapshot saves itself
 *  blocking or deferred. */
case_result_t test_database_save_progress(config_t const& config) {
    std::string const blocking_directory = fmt::format("{}/{}", config.directory(), "saved.blocking/");
    std::string const deferred_directory = fmt::format("{}/{}", config.directory(), "saved.deferred/");
    expect_true(scratch_directory_t::recreate(blocking_directory.c_str()));
    expect_true(scratch_directory_t::recreate(deferred_directory.c_str()));
    auto [database, open_status] = open_database<database_t>(config);
    auto [executor, executor_status] = spawn_executor_handle();
    if (!expect_true(!open_status.failed() && !executor_status.failed())) return {};
    if (!probed(database.supports_snapshots())) return case_result_t::skipped(needs_snapshots_k);
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    triplet_t triplet;
    auto [collection, create_status] = database.create("blobs");
    expect_true(!create_status.failed());
    auto reference = collection[triplet.keys];
    round_trip(reference, triplet);

    // Engines reporting per collection then report before the end, where the order of
    // the counts shows
    auto [named, named_status] = database.create("saved");
    if (!expect_true(!named_status.failed())) return {};
    auto named_reference = named[triplet.keys];
    round_trip(named_reference, triplet);

    std::size_t misordered = 0;
    ustore_size_t last_finished = 0;
    ustore_size_t last_total = 0;
    auto recording = [&](ustore_size_t finished, ustore_size_t total) noexcept {
        misordered += finished < last_finished || finished > total;
        last_finished = finished;
        last_total = total;
        return ustore_continue_k;
    };
    auto [snapshot, snapshot_status] = database.snapshot();
    if (!expect_true(!snapshot_status.failed())) return {};
    expect_true(!snapshot.save(save_target(config, blocking_directory).c_str(), recording).failed());
    expect_true(last_total != 0 && last_finished == last_total);

    last_finished = 0;
    last_total = 0;
    std::string const deferred_target = save_target(config, deferred_directory);
    expect_true(!snapshot.deferred(executor).save(deferred_target.c_str(), recording).wait().failed());
    expect_true(last_total != 0 && last_finished == last_total);
    expect_equal(misordered, 0u);
    return {};
}

/** One thread reads through a snapshot while another replaces it with a newer one. */
case_result_t test_database_snapshot_with_threads(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    if (!probed(database.supports_snapshots())) return case_result_t::skipped(needs_snapshots_k);
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    triplet_t triplet;
    triplet_t same_values_triplet;
    same_values_triplet.values = {'D', 'D', 'D'};

    auto [collection, create_status] = database.create("blobs");
    expect_true(!create_status.failed());
    auto reference = collection[triplet.keys];
    round_trip(reference, triplet);

    auto [snapshot, snapshot_status] = database.snapshot();
    if (!expect_true(!snapshot_status.failed())) return {};
    auto snapshot_reference = blobs_read_collection_t {snapshot.receiver(), collection.handle()}[triplet.keys];

    round_trip(reference, same_values_triplet);

    bool snapshot_locked = false;
    bool is_reset = false;
    auto acquire = [&] {
        return spin_until(
            [&] { return !fu::standard_atomic_ref<bool> {snapshot_locked}.exchange(true, std::memory_order_acquire); });
    };

    auto task = [&](std::size_t thread) noexcept {
        if (thread == 0) {
            while (true) {
                if (!expect_true(acquire(), "the snapshot lock stayed taken")) break;
                if (is_reset) {
                    auto head_reference = blobs_read_collection_t {snapshot.receiver(),
                                                                   collection.handle()}[triplet.keys];
                    check_equalities(head_reference, same_values_triplet);
                    fu::standard_atomic_ref<bool> {snapshot_locked}.store(false, std::memory_order_release);
                    break;
                }
                check_equalities(snapshot_reference, triplet);
                fu::standard_atomic_ref<bool> {snapshot_locked}.store(false, std::memory_order_release);
            }
        }
        else if (expect_true(acquire(), "the snapshot lock stayed taken")) {
            auto [renewed, renewed_status] = database.snapshot();
            expect_true(!renewed_status.failed());
            snapshot = std::move(renewed);
            is_reset = true;
            fu::standard_atomic_ref<bool> {snapshot_locked}.store(false, std::memory_order_release);
        }
    };

    fu::topology_t topology;
    fu::pool_t executor;
    fu::status_t const executor_status = fu::failed(topology.harvest())
                                             ? fu::status_t::topology_unavailable_k
                                             : executor.spawn(topology, 2, fu::caller_inclusive_k);
    if (!expect_true(!fu::failed(executor_status))) return {};
    expect_true(!fu::failed(executor.for_threads(task)));
    return {};
}

/** Concurrent transactions erasing missing keys both commit and leave the database empty. */
case_result_t test_database_transaction_erase_missing(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    if (!probed(database.supports_transactions())) return case_result_t::skipped(needs_transactions_k);
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    expect_true(!database.transaction().status.failed());
    auto [collection, create_status] = database.create("blobs");
    expect_true(!create_status.failed());
    auto [first_transaction, first_transaction_status] = database.transaction();
    if (!expect_true(!first_transaction_status.failed())) return {};
    auto [second_transaction, second_transaction_status] = database.transaction();
    if (!expect_true(!second_transaction_status.failed())) return {};
    blobs_collection_t first_inside {first_transaction.receiver(), collection.handle()};
    blobs_collection_t second_inside {second_transaction.receiver(), collection.handle()};

    expect_true(!second_inside.at(key_from_integer(7297309151944849401)).remove().failed());
    expect_true(!first_inside.at(key_from_integer(8640850744835793378)).remove().failed());
    expect_true(!first_transaction.commit().status.failed());
    expect_true(!second_transaction.commit().status.failed());

    auto [first_value, first_status] = collection.at(key_from_integer(8640850744835793378)).value();
    auto [second_value, second_status] = collection.at(key_from_integer(7297309151944849401)).value();
    expect_true(!first_status.failed() && first_value == value_view_t {});
    expect_true(!second_status.failed() && second_value == value_view_t {});
    expect_true(answers(collection.keys().size(), 0u));
    return {};
}

/** Of two transactions writing the same key, the later commit fails. */
case_result_t test_database_transaction_write_conflicting(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    if (!probed(database.supports_transactions())) return case_result_t::skipped(needs_transactions_k);
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    expect_true(!database.transaction().status.failed());
    auto [collection, create_status] = database.create("blobs");
    expect_true(!create_status.failed());
    auto [first_transaction, first_transaction_status] = database.transaction();
    if (!expect_true(!first_transaction_status.failed())) return {};
    auto [second_transaction, second_transaction_status] = database.transaction();
    if (!expect_true(!second_transaction_status.failed())) return {};

    expect_true(!blobs_collection_t {second_transaction.receiver(), collection.handle()}
                     .at(key_from_integer(6))
                     .assign("a")
                     .failed());
    expect_true(!blobs_collection_t {first_transaction.receiver(), collection.handle()}
                     .at(key_from_integer(6))
                     .assign("b")
                     .failed());
    expect_true(!first_transaction.commit().status.failed());
    expect_true(second_transaction.commit().status.failed());
    return {};
}

/** A stage answers the stamp the transaction reads at, its commit a position no lower, and a later
 *  snapshot reads at or above that position. */
case_result_t test_database_transaction_stamps(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    if (!probed(database.supports_transactions())) return case_result_t::skipped(needs_transactions_k);
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    expect_true(!database.transaction().status.failed());
    auto [transaction, transaction_status] = database.transaction();
    if (!expect_true(!transaction_status.failed())) return {};
    auto [collection, create_status] = database.create("blobs");
    expect_true(!create_status.failed());

    triplet_t triplet;
    auto transaction_reference = blobs_collection_t {transaction.receiver(), collection.handle()}[triplet.keys];

    expect_true(!transaction_reference.assign(triplet.u64_layout()).failed());
    auto [read_stamp, stage_status] = transaction.stage();
    expect_true(!stage_status.failed());
    auto [commit_stamp, commit_status] = transaction.commit();
    expect_true(!commit_status.failed());
    expect_true(static_cast<ustore_stamp_t>(commit_stamp) > 0);
    expect_true(static_cast<ustore_stamp_t>(read_stamp) <= static_cast<ustore_stamp_t>(commit_stamp));
    expect_true(!transaction.reset().status.failed());

    if (!probed(database.supports_snapshots())) return {};
    auto [snapshot, snapshot_status] = database.snapshot();
    expect_true(!snapshot_status.failed());
    expect_true(static_cast<ustore_stamp_t>(commit_stamp) <= static_cast<ustore_stamp_t>(snapshot.read_stamp()));
    return {};
}

/** A database destroyed before its transaction and snapshot stays open for both, each holding it
 *  until it goes, so the transaction still writes, commits and resets, and the snapshot still reads
 *  what it pinned. */
case_result_t test_database_freed_first(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    if (!probed(database.supports_transactions()) && !probed(database.supports_snapshots()))
        return case_result_t::skipped("needs transactions and snapshots");
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    ustore_key_t const key = key_from_integer(91);
    auto [head, create_status] = database.create("blobs");
    expect_true(!create_status.failed());
    expect_true(!head[key].assign("before").failed());

    bool const transacts = probed(database.supports_transactions());
    bool const snapshots = probed(database.supports_snapshots());
    auto [transaction, transaction_status] = transacts ? database.transaction()
                                                       : expected<transaction_t> {{}, status_code_t::unsupported_k};
    auto [snapshot, snapshot_status] = snapshots ? database.snapshot()
                                                 : expected<snapshot_t> {{}, status_code_t::unsupported_k};
    expect_true(!transacts || !transaction_status.failed());
    expect_true(!snapshots || !snapshot_status.failed());
    { database_t const freed = std::move(database); }

    if (!transaction_status.failed()) {
        blobs_collection_t inside {transaction.receiver(), head.handle()};
        expect_true(!inside[key].assign("after").failed());
        expect_true(!transaction.commit().status.failed());
        expect_true(!transaction.reset().status.failed());
        auto [after, after_status] = inside[key].value();
        expect_true(!after_status.failed() && after == "after");
    }
    if (!snapshot_status.failed()) {
        blobs_read_collection_t pinned {snapshot.receiver(), head.handle()};
        auto [before, before_status] = pinned[key].value();
        expect_true(!before_status.failed() && before == "before");
    }
    return {};
}

#pragma endregion Binary Modality

#pragma region Bounded Ranges

/** Writes the keys of the integers [0, 20) under "value" into both collections of @p compared. */
void write_range_keys(compared_collections_t& compared) {
    std::array<ustore_key_t, 20> const keys = keys_of_integers<20>(0);
    for (std::size_t side = 0; side != 2; ++side)
        write_text(compared.databases[side].handle(), compared.collections[side], keys, "value");
}

/** A scan stops before its end key and lists nothing when the end is not past the start. */
void test_blobs_range_end_bound(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_blobs_k);
    if (!expect_true(!compared_status.failed())) return;
    write_range_keys(compared);
    ustore_key_t const start = key_from_integer(5), end = key_from_integer(9), early = key_from_integer(3);
    expect_same_scan(ustore_blobs_scan, compared, {&start, &end, ustore_scan_ascending_k, 10}, keys_of_integers<4>(5));
    expect_same_scan(ustore_blobs_scan, compared, {&start, &end, ustore_scan_ascending_k, 3}, keys_of_integers<3>(5));
    expect_same_scan(ustore_blobs_scan, compared, {nullptr, &early, ustore_scan_ascending_k, 10},
                     keys_of_integers<3>(0));
    expect_same_scan(ustore_blobs_scan, compared, {&end, &start, ustore_scan_ascending_k, 10}, {});
}

/** A descending scan lists its range largest first from the start key down to past the end key. */
void test_blobs_range_descending(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_blobs_k);
    if (!expect_true(!compared_status.failed())) return;
    write_range_keys(compared);
    ustore_key_t const start = key_from_integer(14), end = key_from_integer(4), late = key_from_integer(17);
    expect_same_scan(ustore_blobs_scan, compared, {&start, &end, ustore_scan_descending_k, 20},
                     keys_of_integers<10>(5, ustore_scan_descending_k));
    expect_same_scan(ustore_blobs_scan, compared, {&start, &end, ustore_scan_descending_k, 3},
                     keys_of_integers<3>(12, ustore_scan_descending_k));
    expect_same_scan(ustore_blobs_scan, compared, {nullptr, &late, ustore_scan_descending_k, 5},
                     keys_of_integers<2>(18, ustore_scan_descending_k));
    expect_same_scan(ustore_blobs_scan, compared, {&end, &start, ustore_scan_descending_k, 5}, {});
}

/** A range removal removes the keys from each start up to its end, atomically with other ranges. */
void test_blobs_range_remove(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_blobs_k);
    if (!expect_true(!compared_status.failed())) return;
    write_range_keys(compared);
    std::array<ustore_key_t, 2> const starts {key_from_integer(3), key_from_integer(12)};
    std::array<ustore_key_t, 2> const ends {key_from_integer(7), key_from_integer(14)};
    remove_ranges(ustore_blobs_remove, compared, starts, ends);
    std::array<ustore_key_t, 14> const remaining {
        key_from_integer(0),  key_from_integer(1),  key_from_integer(2),  key_from_integer(7),  key_from_integer(8),
        key_from_integer(9),  key_from_integer(10), key_from_integer(11), key_from_integer(14), key_from_integer(15),
        key_from_integer(16), key_from_integer(17), key_from_integer(18), key_from_integer(19)};
    expect_same_scan(ustore_blobs_scan, compared, {nullptr, nullptr, ustore_scan_ascending_k, 20}, remaining);
}

/** A measure counts the keys of its range with the end excluded, in either direction. */
void test_blobs_range_measure(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_blobs_k);
    if (!expect_true(!compared_status.failed())) return;
    write_range_keys(compared);
    ustore_key_t const lower = key_from_integer(5), upper = key_from_integer(9);
    expect_measure_holds(ustore_blobs_measure, compared, {&lower, &upper, ustore_scan_ascending_k}, 4);
    expect_measure_holds(ustore_blobs_measure, compared, {&upper, &lower, ustore_scan_descending_k}, 4);
    expect_measure_holds(ustore_blobs_measure, compared, {&upper, nullptr, ustore_scan_descending_k}, 10);
    expect_measure_holds(ustore_blobs_measure, compared, {&upper, &lower, ustore_scan_ascending_k}, 0);
}

/** A read with offsets and limits answers each value's byte range, clipped, and its length. */
void test_blobs_byte_range_read(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_blobs_k);
    if (!expect_true(!compared_status.failed())) return;
    write_range_keys(compared);
    std::array<ustore_key_t, 5> const keys {key_from_integer(1), key_from_integer(2), key_from_integer(3),
                                            key_from_integer(4), key_from_integer(40)};
    std::array<ustore_length_t, 5> const offsets {1, 0, 4, 9, 0};
    std::array<ustore_length_t, 5> const limits {3, 2, 10, 1, 4};
    std::array<std::string_view, 5> const pinned {"alu", "va", "e", "", ""};
    for (std::size_t side = 0; side != 2; ++side) {
        read_columns_t columns;
        ustore_length_t const* lengths = nullptr;
        ustore_outcome_t outcome {};
        ustore_blobs_read_t read = read_of(compared.databases[side].handle(), compared.collections[side], keys, columns,
                                           compared.arenas[side].handle(), outcome);
        read.offsets = {offsets.data(), sizeof(ustore_length_t)};
        read.limits = {limits.data(), sizeof(ustore_length_t)};
        read.lengths = &lengths;
        ustore_blobs_read(&read);
        if (!expect_success(outcome)) continue;
        expect_true(lengths && columns.presences && tape_written(columns.values));
        if (!lengths || !columns.presences || !tape_written(columns.values)) continue;
        for (std::size_t index = 0; index != keys.size(); ++index) {
            expect_equal(columns.value(index), pinned[index]);
            expect_equal(static_cast<std::size_t>(lengths[index]), pinned[index].size());
        }
        expect_true(!bits_view_t {columns.presences}[4]);
    }
}

/** What a writer resizing every key of one collection in each of its commits shares with the case
 *  reading them. */
struct resizing_writer_t {
    ustore_database_t database = nullptr;
    ustore_collection_id_t collection {};
    std::span<ustore_key_t const> keys;
    std::uint32_t stopped = 0;
};

/** Rewrites every key of @p writer in one commit after another until stopped, the values of each
 *  commit one letter repeated, short and long by turns. */
void resize_values(resizing_writer_t& writer) noexcept {
    fu::standard_atomic_ref<std::uint32_t const> const stopped {writer.stopped};
    std::string text;
    for (std::size_t round = 0; stopped.load(std::memory_order_acquire) == 0; ++round) {
        text.assign(round % 2 ? 300 : 7, static_cast<char>('a' + round % 26));
        write_text(writer.database, writer.collection, writer.keys, text);
    }
}

/** A batched read racing a writer that resizes the same keys in every commit reads every value at
 *  one stamp, so all of one result come from one commit. */
void test_blobs_read_one_stamp_under_resizes(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    fu::topology_t topology;
    fu::pool_t writing_thread;
    fu::status_t const thread_status = fu::failed(topology.harvest())
                                           ? fu::status_t::topology_unavailable_k
                                           : writing_thread.spawn(topology, 1, fu::caller_exclusive_k);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!open_status.failed() && !fu::failed(thread_status) && !arena_status.failed())) return;
    auto [collection, collection_status] = create_collection(database.handle(), "one_stamp.blobs",
                                                             ustore_modality_blobs_k);
    if (!expect_true(!collection_status.failed())) return;
    std::array<ustore_key_t, 64> const keys = keys_of_integers<64>(0);
    write_text(database.handle(), collection, keys, "seed");

    resizing_writer_t writer {.database = database.handle(), .collection = collection, .keys = keys};
    fu_generation_t generation = 0;
    fu::status_t const start_status = writing_thread.unsafe_for_threads(
        [](fu_lambda_context_t context, std::size_t, std::size_t) noexcept {
            resize_values(*static_cast<resizing_writer_t*>(context));
        },
        &writer, generation);
    expect_true(!fu::failed(start_status));
    std::size_t mixed = 0;
    for (std::size_t round = 0; !fu::failed(start_status) && round != 2000; ++round) {
        read_columns_t columns;
        ustore_outcome_t outcome {};
        ustore_blobs_read_t read = read_of(database.handle(), collection, keys, columns, arena.handle(), outcome);
        ustore_blobs_read(&read);
        if (!expect_success(outcome)) break;
        std::string_view const first = columns.value(0);
        for (std::size_t index = 1; index != keys.size(); ++index) mixed += columns.value(index) != first;
    }
    fu::standard_atomic_ref<std::uint32_t> {writer.stopped}.store(1, std::memory_order_release);
    if (!fu::failed(start_status)) writing_thread.unsafe_join(generation);
    expect_equal(mixed, 0u);
}

/** What a writer moving every value between two halves of one collection's keys shares with the
 *  case scanning them. */
struct swapping_writer_t {
    ustore_database_t database = nullptr;
    ustore_collection_id_t collection {};
    std::span<ustore_key_t const> lower;
    std::span<ustore_key_t const> upper;
    std::uint32_t stopped = 0;
};

/** Writes one half of the keys of @p writer and removes the other in one commit after another
 *  until stopped, the halves swapping every commit. */
void swap_halves(swapping_writer_t& writer) noexcept {
    fu::standard_atomic_ref<std::uint32_t const> const stopped {writer.stopped};
    for (std::size_t round = 0; stopped.load(std::memory_order_acquire) == 0; ++round) {
        std::span<ustore_key_t const> const written = round % 2 ? writer.upper : writer.lower;
        std::span<ustore_key_t const> const removed = round % 2 ? writer.lower : writer.upper;
        ustore_outcome_t outcome {};
        ustore_transaction_t transaction = nullptr;
        ustore_transaction_init_t begin {.transaction = &transaction, .database = writer.database, .outcome = &outcome};
        ustore_transaction_init(&begin);
        if (outcome.status != ustore_status_success_k) continue;
        write_text(writer.database, writer.collection, written, "apple", transaction);
        ustore_blobs_remove_t remove {};
        remove.database = writer.database;
        remove.outcome = &outcome;
        remove.transaction = transaction;
        remove.count = removed.size();
        remove.collections = {&writer.collection, 0};
        remove.keys = {removed.data(), sizeof(ustore_key_t)};
        ustore_blobs_remove(&remove);
        ustore_transaction_commit_t commit {.database = writer.database,
                                            .outcome = &outcome,
                                            .transaction = transaction};
        ustore_transaction_commit(&commit);
        ustore_transaction_free(transaction);
    }
}

/** A scan filtered by the values it lists, racing a writer that moves every value between two
 *  halves of the keys in each commit, lists and reads at one stamp, so it keeps one half. */
case_result_t test_blobs_filtered_scan_one_stamp_under_swaps(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    fu::topology_t topology;
    fu::pool_t writing_thread;
    fu::status_t const thread_status = fu::failed(topology.harvest())
                                           ? fu::status_t::topology_unavailable_k
                                           : writing_thread.spawn(topology, 1, fu::caller_exclusive_k);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!open_status.failed() && !fu::failed(thread_status) && !arena_status.failed())) return {};
    if (!probed(database.supports_transactions())) return case_result_t::skipped(needs_transactions_k);
    auto [collection, collection_status] = create_collection(database.handle(), "swaps.blobs", ustore_modality_blobs_k);
    if (!expect_true(!collection_status.failed())) return {};
    std::array<ustore_key_t, 64> const keys = keys_of_integers<64>(0);
    std::span<ustore_key_t const> const lower {keys.data(), 32}, upper {keys.data() + 32, 32};
    write_text(database.handle(), collection, lower, "apple");

    std::string_view const pattern = "apple";
    std::array<std::uint32_t, 2> const pattern_offsets {0, static_cast<std::uint32_t>(pattern.size())};
    swapping_writer_t writer {.database = database.handle(), .collection = collection, .lower = lower, .upper = upper};
    fu_generation_t generation = 0;
    fu::status_t const start_status = writing_thread.unsafe_for_threads(
        [](fu_lambda_context_t context, std::size_t, std::size_t) noexcept {
            swap_halves(*static_cast<swapping_writer_t*>(context));
        },
        &writer, generation);
    expect_true(!fu::failed(start_status));
    std::size_t torn = 0;
    for (std::size_t round = 0; !fu::failed(start_status) && round != 2000; ++round) {
        ustore_outcome_t outcome {};
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* found = nullptr;
        ustore_blobs_scan_t scan {};
        scan.database = database.handle(), scan.outcome = &outcome, scan.count = 1, scan.collections = {&collection, 0};
        scan.patterns = {.patterns = util::u32_tape(pattern.data(), pattern_offsets.data()),
                         .count = 1,
                         .kind = ustore_pattern_substrings_k,
                         .sensitivity = ustore_cased_k};
        scan.offsets = &offsets, scan.keys = &found, scan.arena = arena.handle(), scan.wanted = keys.size();
        ustore_blobs_scan(&scan);
        expect_success(outcome);
        if (outcome.status != ustore_status_success_k || !offsets) break;
        std::span<ustore_key_t const> const kept {found, static_cast<std::size_t>(offsets[1])};
        bool const one_half = kept.size() == lower.size() && (std::equal(kept.begin(), kept.end(), lower.begin()) ||
                                                              std::equal(kept.begin(), kept.end(), upper.begin()));
        torn += !one_half;
    }
    fu::standard_atomic_ref<std::uint32_t> {writer.stopped}.store(1, std::memory_order_release);
    if (!fu::failed(start_status)) writing_thread.unsafe_join(generation);
    expect_equal(torn, 0u);
    return {};
}

/** A descending cursor with an end key pages its range largest first and stops past the end key. */
void test_blobs_cursor_descending_end(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_blobs_k);
    if (!expect_true(!compared_status.failed())) return;
    write_range_keys(compared);
    ustore_key_t const end = key_from_integer(8);
    std::array<ustore_key_t, 9> const pinned = keys_of_integers<9>(9, ustore_scan_descending_k);
    for (std::size_t side = 0; side != 2; ++side) {
        ustore_blobs_cursor_t cursor = nullptr;
        ustore_outcome_t outcome {};
        ustore_blobs_cursor_init_t init {};
        init.cursor = &cursor;
        init.database = compared.databases[side].handle();
        init.outcome = &outcome;
        init.collection = compared.collections[side];
        init.start_key = key_from_integer(17);
        init.end_key = &end;
        init.direction = ustore_scan_descending_k;
        ustore_blobs_cursor_init(&init);
        if (!expect_success(outcome)) continue;
        std::size_t first = 0;
        for (std::size_t const count : {4, 4, 1, 0}) {
            pulled_keys_t pulled;
            ustore_blobs_cursor_next_t next = pull_of(compared.databases[side].handle(), cursor, pulled,
                                                      compared.arenas[side].handle(), 4, outcome);
            ustore_blobs_cursor_next(&next);
            expect_success(outcome);
            expect_page(pulled, std::span {pinned}.subspan(first, count), "value");
            first += count;
        }
        ustore_blobs_cursor_free(cursor);
    }
}

#pragma endregion Bounded Ranges

/** A sample of one key is uniform over the present keys of runs that overlap, hold removals and
 *  hold several versions of a key: a chi-square over 20 000 samples stays under its 0.001 bound;
 *  and a sample as wide as the present keys holds every one of them. */
void test_blobs_sample_uniform(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "sample_uniform", ustore_modality_blobs_k);
    if (!expect_true(blobs_status.succeeded())) return;
    std::array<ustore_key_t, 72> const keys = keys_of_integers<72>(0);
    std::span<ustore_key_t const> const all {keys};
    ustore_outcome_t outcome {};
    auto const flush = [&]() noexcept {
        ustore_database_flush_t flush {.database = database.handle(), .outcome = &outcome};
        ustore_database_flush(&flush);
    };
    auto const remove = [&](std::span<ustore_key_t const> removed) noexcept {
        ustore_blobs_remove_t remove {};
        remove.database = database.handle();
        remove.outcome = &outcome;
        remove.count = removed.size();
        remove.collections = {&blobs, 0};
        remove.keys = {removed.data(), sizeof(ustore_key_t)};
        ustore_blobs_remove(&remove);
        expect_success(outcome);
    };
    // Flushed runs overlap on 16 to 47 and remove 0 to 7; memory adds 64 to 71, removes 40 to 43
    write_text(database.handle(), blobs, all.subspan(0, 48), "older");
    flush();
    write_text(database.handle(), blobs, all.subspan(16, 48), "newer");
    flush();
    remove(all.subspan(0, 8));
    flush();
    write_text(database.handle(), blobs, all.subspan(64, 8), "fresh");
    remove(all.subspan(40, 4));
    auto const present = [](std::size_t key) noexcept { return key >= 8 && !(key >= 40 && key < 44); };

    constexpr std::size_t samples_k = 20000;
    std::array<std::size_t, 72> hits {};
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(arena_status.succeeded())) return;
    for (std::size_t round = 0; round != samples_k; ++round) {
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* sampled = nullptr;
        ustore_blobs_sample_t sample {};
        sample.database = database.handle();
        sample.outcome = &outcome;
        sample.count = 1;
        sample.collections = {&blobs, 0};
        sample.offsets = &offsets;
        sample.keys = &sampled;
        sample.arena = arena.handle();
        sample.wanted = 1;
        ustore_blobs_sample(&sample);
        if (!expect_success(outcome) || !expect_equal(offsets[1], ustore_length_t {1})) return;
        auto [key, key_status] = util::uuid_to_integer(sampled[0].uuid);
        if (!expect_true(key_status.succeeded() && key < hits.size() && present(key), "sampled key {}", key)) return;
        ++hits[key];
    }
    std::size_t present_count = 0;
    for (std::size_t key = 0; key != hits.size(); ++key) present_count += present(key);
    double const expected = static_cast<double>(samples_k) / static_cast<double>(present_count);
    double chi_square = 0;
    for (std::size_t key = 0; key != hits.size(); ++key)
        if (present(key))
            chi_square += (static_cast<double>(hits[key]) - expected) * (static_cast<double>(hits[key]) - expected) /
                          expected;
    // The 0.999 quantile of chi-square at 59 degrees of freedom, after Wilson and Hilferty
    expect_true(chi_square < 98.4, "chi-square {} over {} present keys", chi_square, present_count);

    // A sample as wide as the present keys, narrower than their versions, holds every present key
    ustore_length_t const* offsets = nullptr;
    ustore_key_t const* sampled = nullptr;
    ustore_blobs_sample_t sample {};
    sample.database = database.handle();
    sample.outcome = &outcome;
    sample.count = 1;
    sample.collections = {&blobs, 0};
    sample.offsets = &offsets;
    sample.keys = &sampled;
    sample.arena = arena.handle();
    sample.wanted = present_count;
    ustore_blobs_sample(&sample);
    if (!expect_success(outcome) || !expect_equal(offsets[1], static_cast<ustore_length_t>(present_count))) return;
    std::array<bool, 72> seen {};
    for (ustore_length_t index = 0; index != offsets[1]; ++index) {
        auto [key, key_status] = util::uuid_to_integer(sampled[index].uuid);
        if (!expect_true(key_status.succeeded() && key < seen.size() && present(key) && !seen[key], "sampled key {}",
                         key))
            return;
        seen[key] = true;
    }
}

/** A batch in shuffled order, split over an executor's slices, lands every value, missing key and
 *  window at its own request index, whatever order the read takes its keys in. */
void test_blobs_read_shuffled(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    auto [blobs, blobs_status] = create_collection(database.handle(), "read_shuffled", ustore_modality_blobs_k);
    if (!expect_true(blobs_status.succeeded())) return;
    auto [executor, executor_status] = spawn_executor();
    if (!expect_true(executor_status.succeeded())) return;
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(arena_status.succeeded())) return void(ustore_executor_free(executor));

    // Keys from 384 up are never written, and every value is its own index spelled out
    constexpr std::size_t written_k = 384;
    constexpr std::size_t count_k = 512;
    std::array<ustore_key_t, written_k> const written = keys_of_integers<written_k>(0);
    std::array<std::array<char, 16>, written_k> texts {};
    std::array<std::string_view, written_k> values {};
    for (std::size_t index = 0; index != written_k; ++index)
        values[index] = {texts[index].data(), fmt::format_to_n(texts[index].data(), 16, "v{}", index).size};
    util::strided_strings<std::string_view> const values_strings {{values.data(), sizeof(std::string_view)}};
    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {};
    write.database = database.handle();
    write.outcome = &outcome;
    write.count = written_k;
    write.collections = {&blobs, 0};
    write.keys = {written.data(), sizeof(ustore_key_t)};
    write.values = values_strings.view();
    ustore_blobs_write(&write);
    expect_success(outcome);

    // A stride coprime with the count shuffles the keys, and odd requests skip their first byte
    std::array<ustore_key_t, count_k> keys {};
    std::array<ustore_length_t, count_k> offsets {};
    for (std::size_t request = 0; request != count_k; ++request) {
        keys[request] = key_from_integer(request * 197 % count_k);
        offsets[request] = static_cast<ustore_length_t>(request % 2);
    }
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database.handle(), blobs, keys, columns, arena.handle(), outcome);
    read.executor = executor;
    read.offsets = {offsets.data(), sizeof(ustore_length_t)};
    ustore_blobs_read(&read);
    if (expect_success(outcome))
        for (std::size_t request = 0; request != count_k; ++request) {
            std::size_t const key = request * 197 % count_k;
            bool const present = (columns.presences[request / 8] >> (request % 8)) & 1u;
            if (!expect_equal(present, key < written_k, "request {}", request) || !present) continue;
            expect_equal(columns.value(request), values[key].substr(request % 2), "request {}", request);
        }
    expect_true(ustore_executor_free(executor) == ustore_status_success_k);
}

int main(int argc, char** argv) {
    start_test_run(argc, argv);

    auto [scratch, scratch_status] = scratch_directory_t::make(test_run.settings.directory);
    if (st::failed(scratch_status)) {
        fmt::println(stderr, "Cannot create a unique directory under {}", test_run.settings.directory);
        return 1;
    }
    char const* const directory = scratch.path();
    config_t const config {test_run.settings.engine, directory};

    run_test_case("test_strided_iterator_postfix_steps", test_strided_iterator_postfix_steps);
    run_test_case(engine_label_t {"test_database_open_clear_close"}.c_str(), test_database_open_clear_close, config);
    run_test_case(engine_label_t {"test_database_clear_collection_by_clearing_database"}.c_str(),
                  test_database_clear_collection_by_clearing_database, config);
    run_test_case(engine_label_t {"test_database_describe"}.c_str(), test_database_describe, config);
    run_test_case(engine_label_t {"test_database_clear_modes"}.c_str(), test_database_clear_modes, config);
    run_test_case(engine_label_t {"test_database_compact"}.c_str(), test_database_compact, config);
    run_test_case(engine_label_t {"test_database_overwrite_with_step"}.c_str(), test_database_overwrite_with_step,
                  config);
    if (directory)
        run_test_case(engine_label_t {"test_database_persistency"}.c_str(), test_database_persistency, config);
    else
        run_test_case(engine_label_t {"test_database_persistency"}.c_str(),
                      [] { return case_result_t::skipped(needs_directory_k); });
    run_test_case(engine_label_t {"test_database_named_collections"}.c_str(), test_database_named_collections, config);
    run_test_case(engine_label_t {"test_database_named_collections_race"}.c_str(), test_database_named_collections_race,
                  config);
    run_test_case(engine_label_t {"test_database_named_collections_list"}.c_str(), test_database_named_collections_list,
                  config);
    run_test_case(engine_label_t {"test_database_collections_resolve"}.c_str(), test_database_collections_resolve,
                  config);
    run_test_case(engine_label_t {"test_database_clear_values"}.c_str(), test_database_clear_values, config);
    run_test_case(engine_label_t {"test_database_presences"}.c_str(), test_database_presences, config);
    run_test_case(engine_label_t {"test_database_batched_read_matches_single"}.c_str(),
                  test_database_batched_read_matches_single, config);
    run_test_case(engine_label_t {"test_database_arena_reuse"}.c_str(), test_database_arena_reuse, config);
    run_test_case(engine_label_t {"test_database_scan"}.c_str(), test_database_scan, config);
    run_test_case(engine_label_t {"test_database_batch_scan"}.c_str(), test_database_batch_scan, config);
    run_test_case(engine_label_t {"test_database_transaction_read_committed"}.c_str(),
                  test_database_transaction_read_committed, config);
    run_test_case(engine_label_t {"test_database_transaction_snapshot_isolation"}.c_str(),
                  test_database_transaction_snapshot_isolation, config);
    run_test_case(engine_label_t {"test_database_transaction_with_snapshot"}.c_str(),
                  test_database_transaction_with_snapshot, config);
    if (directory)
        run_test_case(engine_label_t {"test_database_save_snapshot"}.c_str(), test_database_save_snapshot, config);
    else
        run_test_case(engine_label_t {"test_database_save_snapshot"}.c_str(),
                      [] { return case_result_t::skipped(needs_directory_k); });
    if (directory)
        run_test_case(engine_label_t {"test_database_save_progress"}.c_str(), test_database_save_progress, config);
    else
        run_test_case(engine_label_t {"test_database_save_progress"}.c_str(),
                      [] { return case_result_t::skipped(needs_directory_k); });
    run_test_case(engine_label_t {"test_database_snapshot_with_threads"}.c_str(), test_database_snapshot_with_threads,
                  config);
    run_test_case(engine_label_t {"test_database_transaction_erase_missing"}.c_str(),
                  test_database_transaction_erase_missing, config);
    run_test_case(engine_label_t {"test_database_transaction_write_conflicting"}.c_str(),
                  test_database_transaction_write_conflicting, config);
    run_test_case(engine_label_t {"test_database_transaction_stamps"}.c_str(), test_database_transaction_stamps,
                  config);
    run_test_case(engine_label_t {"test_database_freed_first"}.c_str(), test_database_freed_first, config);
    run_test_case(engine_label_t {"test_blobs_range_end_bound"}.c_str(), test_blobs_range_end_bound, config);
    run_test_case(engine_label_t {"test_blobs_range_descending"}.c_str(), test_blobs_range_descending, config);
    run_test_case(engine_label_t {"test_blobs_range_remove"}.c_str(), test_blobs_range_remove, config);
    run_test_case(engine_label_t {"test_blobs_range_measure"}.c_str(), test_blobs_range_measure, config);
    run_test_case(engine_label_t {"test_blobs_byte_range_read"}.c_str(), test_blobs_byte_range_read, config);
    run_test_case(engine_label_t {"test_blobs_read_one_stamp_under_resizes"}.c_str(),
                  test_blobs_read_one_stamp_under_resizes, config);
    run_test_case(engine_label_t {"test_blobs_filtered_scan_one_stamp_under_swaps"}.c_str(),
                  test_blobs_filtered_scan_one_stamp_under_swaps, config);
    run_test_case(engine_label_t {"test_blobs_cursor_descending_end"}.c_str(), test_blobs_cursor_descending_end,
                  config);
    run_test_case(engine_label_t {"test_blobs_sample_uniform"}.c_str(), test_blobs_sample_uniform, config);
    run_test_case(engine_label_t {"test_blobs_read_shuffled"}.c_str(), test_blobs_read_shuffled, config);
    return finish_test_run();
}
