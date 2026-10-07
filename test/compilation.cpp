/**
 *  @file test/compilation.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Calls the C++ wrappers in each of their accepted forms, so a change that breaks a call site fails to build.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdio>  // `stderr`

#include <array>       // `std::array`
#include <string_view> // `std::string_view`

#include <fmt/format.h>

#include "ustore/ustore.hpp"

#include "test.hpp" // `suite_exit_code`, `expect`, `suite_totals_t`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum;

/** Calls every C++ wrapper in each accepted form against one database. */
std::size_t wrappers_compile(test_config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;

    [[maybe_unused]] auto const main_by_value = database.main();
    blobs_collection_t main_collection = database.main();

    // Single-element access
    failures += expect(main_collection[key_from_integer(42)].assign("purpose of life").succeeded());
    failures += expect(main_collection.at(key_from_integer(42)).assign("purpose of life").succeeded());
    [[maybe_unused]] bool const matches = *main_collection[key_from_integer(42)].value() == "purpose of life";
    [[maybe_unused]] status_t const single_cleared = main_collection[key_from_integer(42)].clear();

    // Many keys to one value
    failures += expect(main_collection[{key_from_integer(43), key_from_integer(44)}].assign("same value").succeeded());

    // Smart references
    [[maybe_unused]] status_t const cleared = main_collection[{key_from_integer(43), key_from_integer(44)}].clear();
    [[maybe_unused]] status_t const erased = main_collection[{key_from_integer(43), key_from_integer(44)}].erase();
    [[maybe_unused]] auto const present = main_collection[{key_from_integer(43), key_from_integer(44)}].present();
    [[maybe_unused]] auto const length = main_collection[{key_from_integer(43), key_from_integer(44)}].length();
    [[maybe_unused]] auto const value = main_collection[{key_from_integer(43), key_from_integer(44)}].value();
    [[maybe_unused]] auto const by_array =
        main_collection[std::array<ustore_key_t, 3> {key_from_integer(65), key_from_integer(66), key_from_integer(67)}];
    std::array<ustore_key_t, 4> const four_keys {key_from_integer(65), key_from_integer(66), key_from_integer(67),
                                                 key_from_integer(68)};
    [[maybe_unused]] auto const by_range = main_collection[strided(four_keys)];
    // A range-for keeps a temporary alive only from C++23, so the answer is named before iterating
    auto const listed = main_collection[{key_from_integer(100), key_from_integer(101)}].value();
    for ([[maybe_unused]] value_view_t const element : *listed) {}

    // Named collections
    if (database.supports_collections()) {
        blobs_collection_t prefixes = *database.find_or_create("prefixes");
        failures += expect(prefixes.at(key_from_integer(42)).assign("purpose").succeeded());
        failures += expect(database["articles"]->at(key_from_integer(42)).assign("of").succeeded());
        failures += expect(database["suffixes"]->at(key_from_integer(42)).assign("life").succeeded());
    }

    // Reusable memory
    arena_t arena;
    [[maybe_unused]] status_t const arena_cleared =
        main_collection[{key_from_integer(43), key_from_integer(44)}].on(arena).clear();
    [[maybe_unused]] status_t const arena_erased =
        main_collection[{key_from_integer(43), key_from_integer(44)}].on(arena).erase();
    [[maybe_unused]] auto const arena_present =
        main_collection[{key_from_integer(43), key_from_integer(44)}].on(arena).present();
    [[maybe_unused]] auto const arena_length =
        main_collection[{key_from_integer(43), key_from_integer(44)}].on(arena).length();
    [[maybe_unused]] auto const arena_value =
        main_collection[{key_from_integer(43), key_from_integer(44)}].on(arena).value();

    // Counting and streaming keys
    [[maybe_unused]] auto const keys_count = main_collection.size();
    [[maybe_unused]] auto const pairs_count =
        main_collection.items(key_from_integer(100), key_from_integer(200)).size();
    auto maybe_stream = main_collection.members(key_from_integer(100), key_from_integer(200)).keys_begin();
    failures += expect(maybe_stream);
    if (maybe_stream)
        for (keys_stream_t& stream = *maybe_stream; !stream.is_end(); ++stream) {
            [[maybe_unused]] ustore_key_t const key = stream.key();
        }
    [[maybe_unused]] auto const cardinality =
        main_collection.members(key_from_integer(100), key_from_integer(200)).size_estimates()->cardinality;

    // Durability and watching
    [[maybe_unused]] status_t const flushed_clear =
        main_collection[{key_from_integer(43), key_from_integer(44)}].on(arena).clear(ustore_durability_flushed_k);
    [[maybe_unused]] status_t const flushed_erase =
        main_collection[{key_from_integer(43), key_from_integer(44)}].on(arena).erase(ustore_durability_flushed_k);
    [[maybe_unused]] auto const untracked_present =
        main_collection[{key_from_integer(43), key_from_integer(44)}].on(arena).present(ustore_watch_none_k);
    [[maybe_unused]] auto const untracked_length =
        main_collection[{key_from_integer(43), key_from_integer(44)}].on(arena).length(ustore_watch_none_k);
    [[maybe_unused]] auto const untracked_value =
        main_collection[{key_from_integer(43), key_from_integer(44)}].on(arena).value(ustore_watch_none_k);

    // Sub-documents
    if (database.supports_collections()) {
        docs_collection_t documents = *database.find_or_create<docs_collection_t>("docs");
        failures += expect(documents[key_from_integer(56)].assign(R"({"hello":"world","answer":42})").succeeded());
        [[maybe_unused]] bool const field_matches =
            documents[collection_key_field_t {key_from_integer(56), "hello"}].value() == "world";
    }

    failures += expect(database.clear().succeeded());
    return failures;
}

int main(int argc, char** argv) {
    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    auto const maybe_directory = test_directory_t::make(options.directory);
    if (!maybe_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", options.directory);
        return 1;
    }
    test_config_t const config {options.engine, maybe_directory->path()};

    report(totals, "wrappers.compile", wrappers_compile, config);
    return suite_exit_code(totals);
}
