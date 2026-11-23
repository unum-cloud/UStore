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

#include <forkunion/atomics.hpp> // `fu::standard_atomic_ref`

#include "ustore/ustore.hpp"

#include "ustore/utilities/ram_device.hpp" // `caller_exclusivity_t`, `ram_executor_t`

#include "abi/fixture.hpp" // `check_length`, `check_equalities`, `round_trip`, `triplet_t`
#include "harness.hpp"     // `run_test_case`, `expect_true`, `expect_equal`, `expect_status`, `spin_until`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::test::abi;
using namespace unum::ustore::utilities;
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
    expect_true(reference.clear_values().succeeded());
    check_length(reference, 0);

    auto maybe_present = collection.members().keys_begin();
    expect_true(maybe_present.has_value());
    if (!maybe_present) return;
    keys_stream_t& present_iterator = *maybe_present;
    for (ustore_key_t const expected_key : triplet.keys) {
        expect_equal(*present_iterator, expected_key);
        ++present_iterator;
    }
    ++present_iterator;
    expect_true(present_iterator.is_end());

    expect_true(reference.remove().succeeded());
    check_length(reference, missing_length_k);
}

/** Opens and clears a database, then round-trips values through a blobs collection. */
void test_database_open_clear_close(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());

    auto [collection, create_status] = database.on().create("blobs");
    expect_true(create_status.succeeded());
    check_binary_collection(collection);
}

/** Clearing the pairs of the whole database empties a blobs collection, and clearing the
 *  collections drops it. */
void test_database_clear_collection_by_clearing_database(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());

    auto [collection, create_status] = database.on().create("blobs");
    expect_true(create_status.succeeded());
    triplet_t triplet;
    auto reference = collection[triplet.keys];
    round_trip(reference, triplet);

    expect_true(collection.keys().size() == 3u);
    expect_true(collection.pairs().size() == 3u);

    expect_true(database.on().clear(ustore_drop_pairs_k).succeeded());
    check_length(reference, missing_length_k);
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());
    expect_true(!*database.on().contains("blobs"));
}

/** A description names the engine the suite opened, whether the head describes itself
 *  blocking or deferred. */
void test_database_describe(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [executor, executor_status] = spawn_executor_handle();
    if (!expect_true(open_status.succeeded() && executor_status.succeeded())) return;

    auto [description, describe_status] = database.on().describe();
    auto [deferred_description, deferred_status] = database.deferred(executor).describe().wait();
    expect_true(describe_status.succeeded() && deferred_status.succeeded());
    expect_equal(std::string_view {ustore_engine_name(description.engine)}, config.engine());
    expect_equal(std::string_view {ustore_engine_name(deferred_description.engine)}, config.engine());
}

/** Clearing the pairs keeps a named collection while emptying it, and a deferred clear of the
 *  collections drops it. */
case_result_t test_database_clear_modes(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [executor, executor_status] = spawn_executor_handle();
    if (!expect_true(open_status.succeeded() && executor_status.succeeded())) return {};
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());
    auto [kept, create_status] = database.on().create("kept");
    if (!expect_true(create_status.succeeded())) return {};
    expect_true(kept[key_from_integer(5)].assign("value").succeeded());

    expect_true(database.on().clear(ustore_drop_pairs_k).succeeded());
    expect_true(*database.on().contains("kept"));
    expect_true(kept.keys().size() == 0u);

    expect_true(database.deferred(executor).clear(ustore_drop_collection_k).wait().succeeded());
    expect_true(!*database.on().contains("kept"));
    return {};
}

/** A compaction its progress callable cancels at the first report answers cancelled, blocking and
 *  deferred alike, and the data still reads. */
void test_database_compact(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [executor, executor_status] = spawn_executor_handle();
    if (!expect_true(open_status.succeeded() && executor_status.succeeded())) return;
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());
    auto [collection, create_status] = database.on().create("blobs");
    expect_true(create_status.succeeded());
    expect_true(collection[key_from_integer(11)].assign("compacted").succeeded());

    std::size_t reports = 0;
    auto cancelling = [&](ustore_size_t, ustore_size_t) noexcept {
        ++reports;
        return ustore_cancel_k;
    };
    expect_status(database.on().compact(cancelling), status_code_t::cancelled_k);
    expect_status(database.deferred(executor).compact(cancelling).wait(), status_code_t::cancelled_k);
    expect_equal(reports, 2u);
    expect_true(database.on().compact().succeeded());
    expect_true(collection[key_from_integer(11)].value() == "compacted");
}

/** Fills keys 1000 to 1100 and 900 down to 800, then overwrites every other key
 *  across both ranges. */
void test_database_overwrite_with_step(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());

    auto [collection, create_status] = database.on().create("blobs");
    expect_true(create_status.succeeded());

    for (std::uint64_t key = 1000; key != 1100; ++key)
        expect_true(collection[key_from_integer(key)].assign("some").succeeded());
    for (std::uint64_t key = 1000; key != 1100; ++key)
        expect_true(*collection[key_from_integer(key)].value() == "some");
    expect_true(collection.keys().size() == 100u);
    expect_true(collection.pairs().size() == 100u);

    for (std::uint64_t key = 900; key != 800; --key)
        expect_true(collection[key_from_integer(key)].assign("other").succeeded());
    for (std::uint64_t key = 900; key != 800; --key) expect_true(*collection[key_from_integer(key)].value() == "other");
    expect_true(collection.keys().size() == 200u);
    expect_true(collection.pairs().size() == 200u);

    for (std::uint64_t key = 800; key != 1100; key += 2)
        expect_true(collection[key_from_integer(key)].assign("third").succeeded());
    for (std::uint64_t key = 800; key != 1100; key += 2)
        expect_true(*collection[key_from_integer(key)].value() == "third");
    expect_true(collection.keys().size() == 250u);
    expect_true(collection.pairs().size() == 250u);
}

/** Populates the database, closes and reopens it, and checks the contents survived. */
void test_database_persistency(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());

    triplet_t triplet;
    {
        auto [kept_collection, kept_status] = database.on().create("kept");
        expect_true(kept_status.succeeded());
        auto kept_reference = kept_collection[triplet.keys];
        check_length(kept_reference, missing_length_k);
        round_trip(kept_reference, triplet);
        check_length(kept_reference, triplet_t::value_size_k);

        auto [emptied_collection, emptied_status] = database.on().create("emptied");
        expect_true(emptied_status.succeeded());
        auto emptied_reference = emptied_collection[triplet.keys];
        check_length(emptied_reference, missing_length_k);
        round_trip(emptied_reference, triplet);
        check_length(emptied_reference, triplet_t::value_size_k);
        expect_true(emptied_collection.clear_values().succeeded());
        check_length(emptied_reference, 0);
    }
    database.close();
    {
        auto [reopened, reopen_status] = open_database<database_t>(config);
        if (!expect_true(reopen_status.succeeded())) return;
        database = std::move(reopened);

        auto [kept_collection, kept_status] = database.on().find("kept");
        expect_true(kept_status.succeeded());
        auto kept_reference = kept_collection[triplet.keys];
        check_equalities(kept_reference, triplet);
        check_length(kept_reference, triplet_t::value_size_k);
        expect_true(kept_collection.keys().size() == 3u);
        expect_true(kept_collection.pairs().size() == 3u);

        expect_true(database.on().contains("emptied").has_value());
        expect_true(*database.on().contains("emptied"));
        auto [emptied_collection, emptied_status] = database.on().find("emptied");
        expect_true(emptied_status.succeeded());
        auto emptied_reference = emptied_collection[triplet.keys];
        check_length(emptied_reference, 0);
        expect_true(emptied_collection.keys().size() == 3u);
        expect_true(emptied_collection.pairs().size() == 3u);
    }
}

/** Creates, looks up, drops and clears collections under unique names. */
void test_database_named_collections(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());

    // A missing collection can't be dropped, and the empty name holds none
    expect_true(!*database.on().contains("unknown"));
    expect_true(database.on().drop("unknown").failed());
    expect_true(database.on().drop("").failed());

    expect_true(database.on().find_or_create("col1").has_value());
    expect_true(database.on().find_or_create("col2").has_value());

    expect_true(!database.on().create("col1"));
    blobs_collection_t first_collection = *database.on().find_or_create("col1");
    expect_true(!database.on().create("col2"));
    blobs_collection_t second_collection = *database.on().find_or_create("col2");

    check_binary_collection(first_collection);
    check_binary_collection(second_collection);

    expect_true(database.on().drop("col1").succeeded());
    expect_true(database.on().drop("col2").succeeded());
    expect_true(!*database.on().contains(""));
    expect_true(!*database.on().contains("col1"));
    expect_true(!*database.on().contains("col2"));

    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());
    expect_true(!*database.on().contains(""));
}

/** Threads racing to create one name leave exactly one collection behind, which
 *  the name addresses. */
case_result_t test_database_named_collections_race(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return {};
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());

    constexpr std::size_t threads_count = 8;
    std::size_t created = 0;
    std::size_t refused = 0;
    auto task = [&](std::size_t) noexcept {
        auto [collection, create_status] = database.on().create("contested");
        if (create_status.succeeded())
            fu::standard_atomic_ref<std::size_t> {created}.store_add(1, std::memory_order_relaxed);
        else if (create_status == status_code_t::already_exists_k)
            fu::standard_atomic_ref<std::size_t> {refused}.store_add(1, std::memory_order_relaxed);
    };
    auto maybe_executor = ram_executor_t::make(threads_count, caller_exclusivity_t::caller_inclusive_k);
    expect_true(maybe_executor.has_value());
    if (!maybe_executor) return {};
    expect_true(st::succeeded(maybe_executor->for_threads(task)));

    expect_equal(created, 1u);
    expect_equal(refused, threads_count - 1);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(arena_status.succeeded())) return {};
    auto [collections, list_status] = database.on().collections(arena);
    auto [found, find_status] = database.on().find("contested");
    expect_true(list_status.succeeded() && find_status.succeeded());
    expect_equal(collections.size(), 1u);
    expect_true(collections.size() == 1 && ustore_collection_id_t(found) == collections[0]);
    expect_true(database.on().drop("contested").succeeded());
    return {};
}

/** Lists every collection with its name and modality beside its identifier, an unnamed one with an
 *  empty name, where each kind of handle creates its own modality and finds no other. */
case_result_t test_database_named_collections_list(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return {};
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());

    // Every kind of collection handle creates a collection of its own modality
    std::array<std::string_view, 3> const names {"col1", "col2", "col3"};
    std::array<ustore_modality_t, 3> const modalities {ustore_modality_blobs_k, ustore_modality_docs_k,
                                                       ustore_modality_graph_k};
    expect_true(database.on().create<blobs_collection_t>(names[0]).has_value());
    expect_true(database.on().create<docs_collection_t>(names[1]).has_value());
    expect_true(database.on().create<graph_collection_t>(names[2]).has_value());
    ustore_outcome_t outcome {};
    ustore_collection_id_t unnamed {};
    ustore_collection_create_t create {};
    create.database = database;
    create.outcome = &outcome;
    create.modality = ustore_modality_strings_k;
    create.collection = &unnamed;
    ustore_collection_create(&create);
    expect_success(outcome);

    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(arena_status.succeeded())) return {};
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
    expect_success(outcome);
    expect_equal(written, 4u);
    if (written != 4) return {};

    // Every listed name resolves back to the identifier beside it, and the unnamed collection lists
    // an empty name
    for (std::size_t index = 0; index != written; ++index) {
        std::string_view const name = string_of(tape_string(listed_names, index));
        if (name.empty()) {
            expect_true(ids[index] == unnamed && listed_modalities[index] == ustore_modality_strings_k);
            continue;
        }
        auto const named = std::ranges::find(names, name);
        expect_true(named != names.end());
        if (named == names.end()) continue;
        expect_true(listed_modalities[index] == modalities[named - names.begin()]);
        auto maybe_found = collection_named(database.receiver(), name);
        expect_true(maybe_found && *maybe_found == ids[index]);
    }

    // A typed lookup refuses a collection of another modality, and a typed creation makes one the
    // graph calls serve
    expect_true(database.on().find<docs_collection_t>("col3") == status_code_t::invalid_argument_k);
    expect_true(database.on().find<graph_collection_t>("col3").has_value());
    auto friends = database.on().find_or_create<graph_collection_t>("friends");
    expect_true(friends && friends->write_edge(edge_t {key_from_integer(1), key_from_integer(2)}).succeeded());

    expect_true(database.on().drop("col1").succeeded());
    expect_true(!*database.on().contains("col1"));
    expect_true(database.on().drop("").failed());
    return {};
}

/** Every call resolves its collections, refusing identifiers nobody created and collections keyed
 *  the other way. */
case_result_t test_database_collections_resolve(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return {};
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());

    // Nothing reaches an identifier the engine never handed out, the nil UUID among them, whatever
    // it keeps there itself
    for (ustore_collection_id_t const unknown :
         {ustore_collection_id_t {}, ustore_collection_id_t {uuid_from_integer(4242)},
          ustore_collection_id_t {largest_key_k.uuid}}) {
        blobs_collection_t keyed {database.receiver(), unknown};
        expect_true(keyed[key_from_integer(1)].value() == status_code_t::not_found_k);
        expect_true(keyed[key_from_integer(1)].assign("intruder") == status_code_t::not_found_k);
        expect_true(keyed.clear() == status_code_t::not_found_k);
        strings_collection_t stringed {database.receiver(), unknown};
        expect_true(stringed.assign("intruder", key_from_integer(1)) == status_code_t::not_found_k);
    }

    // A modality fixes how a collection is keyed, and calls keyed the other way are refused
    auto maybe_words = database.on().create<strings_collection_t>("words");
    auto maybe_numbers = database.on().create("numbers");
    expect_true(maybe_words && maybe_numbers);
    if (!maybe_words || !maybe_numbers) return {};
    ustore_collection_id_t const words = *maybe_words;
    blobs_collection_t keyed_words {database.receiver(), words};
    expect_true(keyed_words[key_from_integer(1)].assign("number") == status_code_t::invalid_argument_k);
    strings_collection_t stringed_numbers {database.receiver(), *maybe_numbers};
    expect_true(stringed_numbers.assign("word", key_from_integer(1)) == status_code_t::invalid_argument_k);
    expect_true(maybe_words->assign("word", key_from_integer(1)).succeeded());

    // A name is never a collection, and dropping a collection by identifier takes its name too
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(arena_status.succeeded())) return {};
    auto [listed, list_status] = database.on().collections(arena);
    expect_true(list_status.succeeded());
    expect_equal(listed.size(), 2u);
    ustore_outcome_t outcome {};
    ustore_collection_drop_t drop {};
    drop.database = database;
    drop.outcome = &outcome;
    drop.collection = words;
    drop.mode = ustore_drop_collection_k;
    ustore_collection_drop(&drop);
    expect_success(outcome);
    expect_true(!*database.on().contains("words"));
    expect_true(database.on().drop("numbers").succeeded());
    return {};
}

/** Clearing values keeps the keys and empties their binary strings. */
void test_database_clear_values(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());

    triplet_t triplet;
    auto [collection, create_status] = database.on().create("blobs");
    expect_true(create_status.succeeded());
    auto reference = collection[triplet.keys];

    check_length(reference, missing_length_k);
    round_trip(reference, triplet);
    check_length(reference, triplet_t::value_size_k);

    expect_true(collection.clear_values().succeeded());
    check_length(reference, 0);
    expect_true(collection.clear().succeeded());
    check_length(reference, missing_length_k);
}

/** Reads presences through the C ABI and the C++ wrappers. */
void test_database_presences(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());
    auto [collection, create_status] = database.on().create("blobs");
    expect_true(create_status.succeeded());

    constexpr std::size_t keys_count = 1000;
    for (std::size_t index = 0; index != keys_count; ++index)
        if (index % 10) expect_true(collection[key_from_integer(index)].assign("value").succeeded());

    std::array<ustore_key_t, keys_count> keys;
    for (std::size_t index = 0; index != keys_count; ++index) keys[index] = key_from_integer(index);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(arena_status.succeeded())) return;
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
    expect_success(outcome);
    expect_true(found_presences != nullptr);
    for (std::size_t index = 0; found_presences && index != keys_count; ++index)
        expect_true(bits_view_t {found_presences}[index] == (index % 10 != 0));

    auto maybe_presences = collection[keys].present();
    expect_true(maybe_presences.has_value());
    auto const& presences = *maybe_presences;
    for (std::size_t index = 0; index != keys_count; ++index)
        expect_true(static_cast<bool>(presences[index]) == (index % 10 != 0));
}

/** One batched read across two collections, missing keys and repeats, answers the very tape,
 *  lengths and presences that reading each key alone answers, before and after a compaction moves
 *  the values out of memory. */
void test_database_batched_read_matches_single(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());
    auto [first, first_status] = database.on().create("first");
    auto [second, second_status] = database.on().create("second");
    if (!expect_true(first_status.succeeded() && second_status.succeeded())) return;

    // Every seventh key stays missing, every eleventh holds an empty value, the rest grow with it.
    constexpr std::size_t keys_count = 3000;
    for (std::size_t index = 0; index != keys_count; ++index) {
        if (index % 7 == 0) continue;
        std::string const value = index % 11 == 0 ? std::string {}
                                                  : std::string(1 + index % 600, char('a' + index % 26));
        auto& collection = index % 2 ? first : second;
        expect_true(collection[key_from_integer(index)]
                        .assign(value_view_t {reinterpret_cast<ustore_byte_t const*>(value.data()), value.size()})
                        .succeeded());
    }
    std::array<ustore_key_t, keys_count + 64> keys;
    std::array<ustore_collection_id_t, keys_count + 64> collections;
    for (std::size_t index = 0; index != keys.size(); ++index) {
        std::size_t const key = (index * 7919) % keys_count;
        keys[index] = key_from_integer(key);
        collections[index] = key % 2 ? *first.member_ptr() : *second.member_ptr();
    }
    auto [arena, arena_status] = arena_t::make();
    auto [single_arena, single_arena_status] = arena_t::make();
    if (!expect_true(arena_status.succeeded() && single_arena_status.succeeded())) return;

    auto compare = [&]() {
        ustore_outcome_t outcome {};
        ustore_octet_t const* presences = nullptr;
        ustore_length_t const* lengths = nullptr;
        ustore_strings_view_t tape {};
        ustore_blobs_read_t read {.database = database,
                                  .outcome = &outcome,
                                  .count = keys.size(),
                                  .collections = {collections.data(), sizeof(ustore_collection_id_t)},
                                  .keys = {keys.data(), sizeof(ustore_key_t)},
                                  .presences = &presences,
                                  .lengths = &lengths,
                                  .values = &tape,
                                  .arena = arena};
        ustore_blobs_read(&read);
        if (!expect_success(outcome)) return;
        std::size_t mismatches = 0;
        for (std::size_t index = 0; index != keys.size(); ++index) {
            ustore_octet_t const* single_presences = nullptr;
            ustore_length_t const* single_lengths = nullptr;
            ustore_strings_view_t single_tape {};
            ustore_blobs_read_t alone {.database = database,
                                       .outcome = &outcome,
                                       .count = 1,
                                       .collections = {&collections[index], 0},
                                       .keys = {&keys[index], 0},
                                       .presences = &single_presences,
                                       .lengths = &single_lengths,
                                       .values = &single_tape,
                                       .arena = single_arena};
            ustore_blobs_read(&alone);
            if (outcome.status != ustore_status_success_k) {
                ++mismatches;
                continue;
            }
            std::string_view const batched = string_of(tape_string(tape, index));
            std::string_view const single = string_of(tape_string(single_tape, 0));
            mismatches += batched != single || lengths[index] != single_lengths[0] ||
                          bits_view_t {presences}[index] != bits_view_t {single_presences}[0];
        }
        expect_equal(mismatches, std::size_t {0});
    };
    compare();
    expect_true(database.on().compact().succeeded());
    compare();
}

/** One arena serves the reads of two collections in turn, each read rewinding it, while shorthand
 *  reads keep no arena anywhere and own the one-off arena they answer from. */
case_result_t test_database_arena_reuse(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return {};
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());

    auto [first, first_status] = database.on().create("first");
    auto [second, second_status] = database.on().create("second");
    if (!expect_true(first_status.succeeded() && second_status.succeeded())) return {};
    std::array<ustore_key_t, 2> const keys {key_from_integer(1), key_from_integer(2)};
    std::array<value_view_t, 2> const first_values {"alpha", "beta"};
    std::array<value_view_t, 2> const second_values {"gamma", "a much longer delta"};
    expect_true(first[keys].assign(first_values).succeeded());
    expect_true(second[keys].assign(second_values).succeeded());

    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(arena_status.succeeded())) return {};
    for (std::size_t round = 0; round != 2; ++round) {
        auto [from_first, first_read_status] = first.read(keys, arena);
        expect_true(first_read_status.succeeded());
        for (std::size_t index = 0; first_read_status.succeeded() && index != keys.size(); ++index)
            expect_true(from_first[index] == first_values[index]);

        auto [from_second, second_read_status] = second.read(keys, arena);
        expect_true(second_read_status.succeeded());
        for (std::size_t index = 0; second_read_status.succeeded() && index != keys.size(); ++index)
            expect_true(from_second[index] == second_values[index]);
    }

    // The shorthand answers outlive the collection handles that read them, since each owns
    // its own arena
    expected<owned_value_t> kept = first[keys[0]].value();
    expected<owned<values_view_t>> kept_batch = second[keys].value();
    first = blobs_collection_t {};
    second = blobs_collection_t {};
    expect_true(kept == first_values[0]);
    expect_true(kept_batch && kept_batch->size() == 2 && (*kept_batch)[1] == second_values[1]);
    expect_true(database.on().drop("first").succeeded() && database.on().drop("second").succeeded());
    return {};
}

/** A key stream visits every written key in order. */
void test_database_scan(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());
    auto [collection, create_status] = database.on().create("blobs");
    expect_true(create_status.succeeded());

    constexpr std::size_t keys_count = 1000;
    std::array<ustore_key_t, keys_count> keys;
    for (std::size_t index = 0; index != keys_count; ++index) keys[index] = key_from_integer(index);
    auto reference = collection[keys];
    value_view_t const value("value");
    expect_true(reference.assign(value).succeeded());
    auto [stream, stream_status] = keys_stream_t::make(database.receiver(), collection, smallest_key_k, 256);
    expect_true(stream_status.succeeded());
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
    if (!expect_true(open_status.succeeded())) return;
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());
    auto [collection, create_status] = database.on().create("blobs");
    expect_true(create_status.succeeded());

    std::array<ustore_key_t, 512> keys;
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = key_from_integer(index);
    auto reference = collection[keys];
    value_view_t const value("value");
    expect_true(reference.assign(value).succeeded());
    auto [stream, stream_status] = keys_stream_t::make(database.receiver(), collection, smallest_key_k, 256);
    expect_true(stream_status.succeeded());
    auto batch = stream.keys_batch();
    expect_equal(batch.size(), 256u);
    expect_true(!stream.is_end());
    for (std::size_t index = 0; index != 256; ++index) expect_equal(batch[index], key_from_integer(index));

    expect_true(stream.seek_to_next_batch().succeeded());
    batch = stream.keys_batch();
    expect_equal(batch.size(), 256u);
    expect_true(!stream.is_end());
    for (std::size_t index = 0; index != 256; ++index) expect_equal(batch[index], key_from_integer(index + 256));

    expect_true(stream.seek_to_next_batch().succeeded());
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
    if (!expect_true(open_status.succeeded())) return {};
    if (!database.on().supports_transactions()) return case_result_t::skipped(needs_transactions_k);
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());
    expect_true(database.on().transaction().has_value());
    transaction_t transaction = *database.on().transaction();
    auto [collection, create_status] = database.on().create("blobs");
    expect_true(create_status.succeeded());

    triplet_t triplet;
    auto transaction_reference = blobs_collection_t {transaction.receiver(), collection}[triplet.keys];
    round_trip(transaction_reference, triplet);

    auto collection_reference = collection[triplet.keys];

    check_length(collection_reference, missing_length_k);
    expect_true(transaction.on().commit().has_value());
    expect_true(transaction.on().reset().has_value());

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
    if (!expect_true(open_status.succeeded())) return {};
    if (!database.on().supports_snapshots()) return case_result_t::skipped(needs_snapshots_k);
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());

    triplet_t triplet;
    triplet_t same_values_triplet;
    same_values_triplet.values = {'D', 'D', 'D'};

    auto [collection, create_status] = database.on().create("blobs");
    expect_true(create_status.succeeded());
    auto collection_reference = collection[triplet.keys];

    check_length(collection_reference, missing_length_k);
    round_trip(collection_reference, triplet);

    auto snapshot = *database.on().snapshot();
    auto snapshot_reference = blobs_read_collection_t {snapshot.receiver(), collection}[triplet.keys];
    check_equalities(snapshot_reference, triplet);
    round_trip(collection_reference, same_values_triplet);

    auto maybe_retrieved = snapshot_reference.value();
    expect_true(maybe_retrieved.has_value());
    if (!maybe_retrieved) return {};
    auto const& retrieved = *maybe_retrieved;
    auto iterator = retrieved.begin();
    for (value_view_t const newer_view : same_values_triplet.views()) {
        value_view_t const retrieved_view = *iterator;
        expect_equal(retrieved_view.size(), newer_view.size());
        expect_true(retrieved_view != newer_view);
        ++iterator;
    }

    snapshot = *database.on().snapshot();
    auto reference = blobs_read_collection_t {snapshot.receiver(), collection}[same_values_triplet.keys];
    check_equalities(reference, same_values_triplet);
    return {};
}

/** A transaction reads the head while a snapshot reads its own view. */
case_result_t test_database_transaction_with_snapshot(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return {};
    if (!database.on().supports_snapshots()) return case_result_t::skipped(needs_snapshots_k);
    if (!database.on().supports_transactions()) return case_result_t::skipped(needs_transactions_k);
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());

    triplet_t triplet;
    triplet_t same_values_triplet;
    same_values_triplet.values = {'D', 'D', 'D'};

    auto [collection, create_status] = database.on().create("blobs");
    expect_true(create_status.succeeded());
    auto collection_reference = collection[triplet.keys];

    check_length(collection_reference, missing_length_k);
    round_trip(collection_reference, triplet);

    auto snapshot = *database.on().snapshot();
    auto snapshot_reference = blobs_read_collection_t {snapshot.receiver(), collection}[triplet.keys];
    check_equalities(snapshot_reference, triplet);

    round_trip(collection_reference, same_values_triplet);

    transaction_t transaction = *database.on().transaction();
    auto head_reference = blobs_collection_t {transaction.receiver(), collection}[triplet.keys];
    check_equalities(head_reference, same_values_triplet);
    auto older_reference = blobs_read_collection_t {snapshot.receiver(), collection}[triplet.keys];
    check_equalities(older_reference, triplet);

    snapshot = *database.on().snapshot();
    auto newer_reference = blobs_read_collection_t {snapshot.receiver(), collection}[triplet.keys];
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
    if (!expect_true(open_status.succeeded())) return {};
    if (!database.on().supports_snapshots()) return case_result_t::skipped(needs_snapshots_k);
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());

    triplet_t triplet;
    triplet_t same_values_triplet;
    same_values_triplet.values = {'D', 'D', 'D'};

    auto [collection, create_status] = database.on().create("blobs");
    expect_true(create_status.succeeded());
    auto collection_reference = collection[triplet.keys];
    round_trip(collection_reference, triplet);

    auto first_snapshot = *database.on().snapshot();
    expect_true(first_snapshot.on().save(save_target(config, first_saved_directory).c_str()).succeeded());
    check_equalities(collection_reference, triplet);

    config_t const first_config {config.options(), first_saved_directory.c_str()};
    auto [first_saved, first_saved_status] = open_database<database_t>(first_config);
    if (!expect_true(first_saved_status.succeeded())) return {};
    auto [first_collection, first_find_status] = first_saved.on().find("blobs");
    expect_true(first_find_status.succeeded());
    auto first_reference = first_collection[triplet.keys];
    check_equalities(first_reference, triplet);

    round_trip(collection_reference, same_values_triplet);

    auto second_snapshot = *database.on().snapshot();
    expect_true(second_snapshot.on().save(save_target(config, second_saved_directory).c_str()).succeeded());
    check_equalities(collection_reference, same_values_triplet);

    config_t const second_config {config.options(), second_saved_directory.c_str()};
    auto [second_saved, second_saved_status] = open_database<database_t>(second_config);
    if (!expect_true(second_saved_status.succeeded())) return {};
    auto [second_collection, second_find_status] = second_saved.on().find("blobs");
    expect_true(second_find_status.succeeded());
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
    if (!expect_true(open_status.succeeded() && executor_status.succeeded())) return {};
    if (!database.on().supports_snapshots()) return case_result_t::skipped(needs_snapshots_k);
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());
    triplet_t triplet;
    auto [collection, create_status] = database.on().create("blobs");
    expect_true(create_status.succeeded());
    auto reference = collection[triplet.keys];
    round_trip(reference, triplet);

    // Engines reporting per collection then report before the end, where the order of
    // the counts shows
    auto [named, named_status] = database.on().create("saved");
    if (!expect_true(named_status.succeeded())) return {};
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
    auto snapshot = *database.on().snapshot();
    expect_true(snapshot.on().save(save_target(config, blocking_directory).c_str(), recording).succeeded());
    expect_true(last_total != 0 && last_finished == last_total);

    last_finished = 0;
    last_total = 0;
    std::string const deferred_target = save_target(config, deferred_directory);
    expect_true(snapshot.deferred(executor).save(deferred_target.c_str(), recording).wait().succeeded());
    expect_true(last_total != 0 && last_finished == last_total);
    expect_equal(misordered, 0u);
    return {};
}

/** One thread reads through a snapshot while another replaces it with a newer one. */
case_result_t test_database_snapshot_with_threads(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return {};
    if (!database.on().supports_snapshots()) return case_result_t::skipped(needs_snapshots_k);
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());

    triplet_t triplet;
    triplet_t same_values_triplet;
    same_values_triplet.values = {'D', 'D', 'D'};

    auto [collection, create_status] = database.on().create("blobs");
    expect_true(create_status.succeeded());
    auto reference = collection[triplet.keys];
    round_trip(reference, triplet);

    auto snapshot = *database.on().snapshot();
    auto snapshot_reference = blobs_read_collection_t {snapshot.receiver(), collection}[triplet.keys];

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
                    auto head_reference = blobs_read_collection_t {snapshot.receiver(), collection}[triplet.keys];
                    check_equalities(head_reference, same_values_triplet);
                    fu::standard_atomic_ref<bool> {snapshot_locked}.store(false, std::memory_order_release);
                    break;
                }
                check_equalities(snapshot_reference, triplet);
                fu::standard_atomic_ref<bool> {snapshot_locked}.store(false, std::memory_order_release);
            }
        }
        else if (expect_true(acquire(), "the snapshot lock stayed taken")) {
            snapshot = *database.on().snapshot();
            is_reset = true;
            fu::standard_atomic_ref<bool> {snapshot_locked}.store(false, std::memory_order_release);
        }
    };

    auto maybe_executor = ram_executor_t::make(2, caller_exclusivity_t::caller_inclusive_k);
    if (!expect_true(maybe_executor.has_value())) return {};
    expect_true(st::succeeded(maybe_executor->for_threads(task)));
    return {};
}

/** Concurrent transactions erasing missing keys both commit and leave the database empty. */
case_result_t test_database_transaction_erase_missing(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return {};
    if (!database.on().supports_transactions()) return case_result_t::skipped(needs_transactions_k);
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());
    expect_true(database.on().transaction().has_value());
    auto [collection, create_status] = database.on().create("blobs");
    expect_true(create_status.succeeded());
    transaction_t first_transaction = *database.on().transaction();
    transaction_t second_transaction = *database.on().transaction();
    blobs_collection_t first_inside {first_transaction.receiver(), collection};
    blobs_collection_t second_inside {second_transaction.receiver(), collection};

    expect_true(second_inside.at(key_from_integer(7297309151944849401)).remove().succeeded());
    expect_true(first_inside.at(key_from_integer(8640850744835793378)).remove().succeeded());
    expect_true(first_transaction.on().commit().has_value());
    expect_true(second_transaction.on().commit().has_value());

    expect_true(collection.at(key_from_integer(8640850744835793378)).value() == value_view_t {});
    expect_true(collection.at(key_from_integer(7297309151944849401)).value() == value_view_t {});
    expect_true(collection.keys().size() == 0u);
    return {};
}

/** Of two transactions writing the same key, the later commit fails. */
case_result_t test_database_transaction_write_conflicting(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return {};
    if (!database.on().supports_transactions()) return case_result_t::skipped(needs_transactions_k);
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());
    expect_true(database.on().transaction().has_value());
    auto [collection, create_status] = database.on().create("blobs");
    expect_true(create_status.succeeded());
    transaction_t first_transaction = *database.on().transaction();
    transaction_t second_transaction = *database.on().transaction();

    expect_true(
        blobs_collection_t {second_transaction.receiver(), collection}.at(key_from_integer(6)).assign("a").succeeded());
    expect_true(
        blobs_collection_t {first_transaction.receiver(), collection}.at(key_from_integer(6)).assign("b").succeeded());
    expect_true(first_transaction.on().commit().has_value());
    expect_true(second_transaction.on().commit().failed());
    return {};
}

/** A stage answers the stamp the transaction reads at, its commit a position no lower, and a later
 *  snapshot reads at or above that position. */
case_result_t test_database_transaction_stamps(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return {};
    if (!database.on().supports_transactions()) return case_result_t::skipped(needs_transactions_k);
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());

    expect_true(database.on().transaction().has_value());
    transaction_t transaction = *database.on().transaction();
    auto [collection, create_status] = database.on().create("blobs");
    expect_true(create_status.succeeded());

    triplet_t triplet;
    auto transaction_reference = blobs_collection_t {transaction.receiver(), collection}[triplet.keys];

    expect_true(transaction_reference.assign(triplet.u64_layout()).succeeded());
    auto [read_stamp, stage_status] = transaction.on().stage();
    expect_true(stage_status.succeeded());
    auto [commit_stamp, commit_status] = transaction.on().commit();
    expect_true(commit_status.succeeded());
    expect_true(static_cast<ustore_stamp_t>(commit_stamp) > 0);
    expect_true(static_cast<ustore_stamp_t>(read_stamp) <= static_cast<ustore_stamp_t>(commit_stamp));
    expect_true(transaction.on().reset().has_value());

    if (!database.on().supports_snapshots()) return {};
    auto [snapshot, snapshot_status] = database.on().snapshot();
    expect_true(snapshot_status.succeeded());
    expect_true(static_cast<ustore_stamp_t>(commit_stamp) <= static_cast<ustore_stamp_t>(snapshot.read_stamp()));
    return {};
}

/** A database destroyed before its transaction and snapshot stays open for both, each holding it
 *  until it goes, so the transaction still writes, commits and resets, and the snapshot still reads
 *  what it pinned. */
case_result_t test_database_freed_first(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return {};
    if (!database.on().supports_transactions() && !database.on().supports_snapshots())
        return case_result_t::skipped("needs transactions and snapshots");
    expect_true(database.on().clear(ustore_drop_collection_k).succeeded());
    ustore_key_t const key = key_from_integer(91);
    auto [head, create_status] = database.on().create("blobs");
    expect_true(create_status.succeeded());
    expect_true(head[key].assign("before").succeeded());

    expected<transaction_t> transaction = status_code_t::unsupported_k;
    expected<snapshot_t> snapshot = status_code_t::unsupported_k;
    if (database.on().supports_transactions()) {
        transaction = database.on().transaction();
        expect_true(transaction.has_value());
    }
    if (database.on().supports_snapshots()) {
        snapshot = database.on().snapshot();
        expect_true(snapshot.has_value());
    }
    { database_t const freed = std::move(database); }

    if (transaction) {
        blobs_collection_t inside {transaction->receiver(), head};
        expect_true(inside[key].assign("after").succeeded());
        expect_true(transaction->on().commit().has_value());
        expect_true(transaction->on().reset().has_value());
        expect_true(inside[key].value() == "after");
    }
    if (snapshot) {
        blobs_read_collection_t pinned {snapshot->receiver(), head};
        expect_true(pinned[key].value() == "before");
    }
    return {};
}

#pragma endregion Binary Modality

#pragma region Bounded Ranges

/** Writes the keys of the integers [0, 20) under "value" into both collections of @p compared. */
void write_range_keys(compared_collections_t& compared) {
    std::array<ustore_key_t, 20> const keys = keys_of_integers<20>(0);
    for (std::size_t side = 0; side != 2; ++side)
        write_text(compared.databases[side], compared.collections[side], keys, "value");
}

/** A scan stops before its end key and lists nothing when the end is not past the start. */
void test_blobs_range_end_bound(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_blobs_k);
    if (!expect_true(compared_status.succeeded())) return;
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
    if (!expect_true(compared_status.succeeded())) return;
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
    if (!expect_true(compared_status.succeeded())) return;
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
    if (!expect_true(compared_status.succeeded())) return;
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
    if (!expect_true(compared_status.succeeded())) return;
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
        ustore_blobs_read_t read = read_of(compared.databases[side], compared.collections[side], keys, columns,
                                           compared.arenas[side], outcome);
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
    st::expected<ram_executor_t> writing_thread = ram_executor_t::make(1, caller_exclusivity_t::caller_exclusive_k);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(open_status.succeeded() && writing_thread.has_value() && arena_status.succeeded())) return;
    auto [collection, collection_status] = create_collection(database, "one_stamp.blobs", ustore_modality_blobs_k);
    if (!expect_true(collection_status.succeeded())) return;
    std::array<ustore_key_t, 64> const keys = keys_of_integers<64>(0);
    write_text(database, collection, keys, "seed");

    resizing_writer_t writer {.database = database, .collection = collection, .keys = keys};
    st::expected<fu_generation_t> const generation = writing_thread->start<&resize_values>(writer);
    expect_true(generation.has_value());
    std::size_t mixed = 0;
    for (std::size_t round = 0; generation && round != 2000; ++round) {
        read_columns_t columns;
        ustore_outcome_t outcome {};
        ustore_blobs_read_t read = read_of(database, collection, keys, columns, arena, outcome);
        ustore_blobs_read(&read);
        if (!expect_success(outcome)) break;
        std::string_view const first = columns.value(0);
        for (std::size_t index = 1; index != keys.size(); ++index) mixed += columns.value(index) != first;
    }
    fu::standard_atomic_ref<std::uint32_t> {writer.stopped}.store(1, std::memory_order_release);
    if (generation) writing_thread->join(*generation);
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
    st::expected<ram_executor_t> writing_thread = ram_executor_t::make(1, caller_exclusivity_t::caller_exclusive_k);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(open_status.succeeded() && writing_thread.has_value() && arena_status.succeeded())) return {};
    if (!database.on().supports_transactions()) return case_result_t::skipped(needs_transactions_k);
    auto [collection, collection_status] = create_collection(database, "swaps.blobs", ustore_modality_blobs_k);
    if (!expect_true(collection_status.succeeded())) return {};
    std::array<ustore_key_t, 64> const keys = keys_of_integers<64>(0);
    std::span<ustore_key_t const> const lower {keys.data(), 32}, upper {keys.data() + 32, 32};
    write_text(database, collection, lower, "apple");

    std::string_view const pattern = "apple";
    std::array<std::uint32_t, 2> const pattern_offsets {0, static_cast<std::uint32_t>(pattern.size())};
    swapping_writer_t writer {.database = database, .collection = collection, .lower = lower, .upper = upper};
    st::expected<fu_generation_t> const generation = writing_thread->start<&swap_halves>(writer);
    expect_true(generation.has_value());
    std::size_t torn = 0;
    for (std::size_t round = 0; generation && round != 2000; ++round) {
        ustore_outcome_t outcome {};
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* found = nullptr;
        ustore_blobs_scan_t scan {};
        scan.database = database, scan.outcome = &outcome, scan.count = 1, scan.collections = {&collection, 0};
        scan.patterns = {.patterns = u32_tape(pattern.data(), pattern_offsets.data()),
                         .count = 1,
                         .kind = ustore_pattern_substrings_k,
                         .sensitivity = ustore_cased_k};
        scan.offsets = &offsets, scan.keys = &found, scan.arena = arena, scan.wanted = keys.size();
        ustore_blobs_scan(&scan);
        expect_success(outcome);
        if (outcome.status != ustore_status_success_k || !offsets) break;
        std::span<ustore_key_t const> const kept {found, static_cast<std::size_t>(offsets[1])};
        bool const one_half = kept.size() == lower.size() && (std::equal(kept.begin(), kept.end(), lower.begin()) ||
                                                              std::equal(kept.begin(), kept.end(), upper.begin()));
        torn += !one_half;
    }
    fu::standard_atomic_ref<std::uint32_t> {writer.stopped}.store(1, std::memory_order_release);
    if (generation) writing_thread->join(*generation);
    expect_equal(torn, 0u);
    return {};
}

/** A descending cursor with an end key pages its range largest first and stops past the end key. */
void test_blobs_cursor_descending_end(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_blobs_k);
    if (!expect_true(compared_status.succeeded())) return;
    write_range_keys(compared);
    ustore_key_t const end = key_from_integer(8);
    std::array<ustore_key_t, 9> const pinned = keys_of_integers<9>(9, ustore_scan_descending_k);
    for (std::size_t side = 0; side != 2; ++side) {
        ustore_blobs_cursor_t cursor = nullptr;
        ustore_outcome_t outcome {};
        ustore_blobs_cursor_init_t init {};
        init.cursor = &cursor;
        init.database = compared.databases[side];
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
            ustore_blobs_cursor_next_t next = pull_of(compared.databases[side], cursor, pulled, compared.arenas[side],
                                                      4, outcome);
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
    auto [blobs, blobs_status] = create_collection(database, "sample_uniform", ustore_modality_blobs_k);
    if (!expect_true(blobs_status.succeeded())) return;
    std::array<ustore_key_t, 72> const keys = keys_of_integers<72>(0);
    std::span<ustore_key_t const> const all {keys};
    ustore_outcome_t outcome {};
    auto const flush = [&]() noexcept {
        ustore_database_flush_t flush {.database = database, .outcome = &outcome};
        ustore_database_flush(&flush);
    };
    auto const remove = [&](std::span<ustore_key_t const> removed) noexcept {
        ustore_blobs_remove_t remove {};
        remove.database = database;
        remove.outcome = &outcome;
        remove.count = removed.size();
        remove.collections = {&blobs, 0};
        remove.keys = {removed.data(), sizeof(ustore_key_t)};
        ustore_blobs_remove(&remove);
        expect_success(outcome);
    };
    // Flushed runs overlap on 16 to 47 and remove 0 to 7; memory adds 64 to 71, removes 40 to 43
    write_text(database, blobs, all.subspan(0, 48), "older");
    flush();
    write_text(database, blobs, all.subspan(16, 48), "newer");
    flush();
    remove(all.subspan(0, 8));
    flush();
    write_text(database, blobs, all.subspan(64, 8), "fresh");
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
        sample.database = database;
        sample.outcome = &outcome;
        sample.count = 1;
        sample.collections = {&blobs, 0};
        sample.offsets = &offsets;
        sample.keys = &sampled;
        sample.arena = arena;
        sample.wanted = 1;
        ustore_blobs_sample(&sample);
        if (!expect_success(outcome) || !expect_equal(offsets[1], ustore_length_t {1})) return;
        std::size_t const key = static_cast<std::size_t>(*uuid_to_integer(sampled[0].uuid));
        if (!expect_true(key < hits.size() && present(key), "sampled key {}", key)) return;
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
    sample.database = database;
    sample.outcome = &outcome;
    sample.count = 1;
    sample.collections = {&blobs, 0};
    sample.offsets = &offsets;
    sample.keys = &sampled;
    sample.arena = arena;
    sample.wanted = present_count;
    ustore_blobs_sample(&sample);
    if (!expect_success(outcome) || !expect_equal(offsets[1], static_cast<ustore_length_t>(present_count))) return;
    std::array<bool, 72> seen {};
    for (ustore_length_t index = 0; index != offsets[1]; ++index) {
        std::size_t const key = static_cast<std::size_t>(*uuid_to_integer(sampled[index].uuid));
        if (!expect_true(key < seen.size() && present(key) && !seen[key], "sampled key {}", key)) return;
        seen[key] = true;
    }
}

/** A batch in shuffled order, split over an executor's slices, lands every value, missing key and
 *  window at its own request index, whatever order the read takes its keys in. */
void test_blobs_read_shuffled(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    auto [blobs, blobs_status] = create_collection(database, "read_shuffled", ustore_modality_blobs_k);
    if (!expect_true(blobs_status.succeeded())) return;
    auto [executor, executor_status] = spawn_executor();
    if (!expect_true(executor_status.succeeded())) return;
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(arena_status.succeeded())) return ustore_executor_free(executor);

    // Keys from 384 up are never written, and every value is its own index spelled out
    constexpr std::size_t written_k = 384;
    constexpr std::size_t count_k = 512;
    std::array<ustore_key_t, written_k> const written = keys_of_integers<written_k>(0);
    std::array<std::array<char, 16>, written_k> texts {};
    std::array<std::string_view, written_k> values {};
    for (std::size_t index = 0; index != written_k; ++index)
        values[index] = {texts[index].data(), fmt::format_to_n(texts[index].data(), 16, "v{}", index).size};
    strided_strings<std::string_view> const values_strings {{values.data(), sizeof(std::string_view)}};
    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {};
    write.database = database;
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
    ustore_blobs_read_t read = read_of(database, blobs, keys, columns, arena, outcome);
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
    ustore_executor_free(executor);
}

int main(int argc, char** argv) {
    start_test_run(argc, argv);

    auto const maybe_directory = scratch_directory_t::make(test_run.settings.directory);
    if (!maybe_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", test_run.settings.directory);
        return 1;
    }
    char const* const directory = maybe_directory->path();
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
