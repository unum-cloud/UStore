/**
 *  @file test/compilation.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Calls the C++ wrappers in each of their accepted forms, so a change that breaks a call site fails to build.
 */

#include <cstddef> // `std::size_t`
#include <cstdio>  // `stderr`

#include <array>       // `std::array`
#include <string_view> // `std::string_view`

#include <fmt/format.h>

#include <ustore/ustore.hpp>

#include "test.hpp"

using namespace unum::ustore;
using namespace unum;

/** Calls every C++ wrapper in each accepted form against one database. */
std::size_t wrappers_compile(char const* directory) {
    std::size_t failures = 0;
    test_config_t const config {directory};
    database_t database;
    failures += expect(database.open(config.c_str()).succeeded());

    [[maybe_unused]] auto const main_by_value = database.main();
    blobs_collection_t main_collection = database.main();

    // Single-element access
    failures += expect(main_collection[42].assign("purpose of life").succeeded());
    failures += expect(main_collection.at(42).assign("purpose of life").succeeded());
    [[maybe_unused]] bool const matches = *main_collection[42].value() == "purpose of life";
    [[maybe_unused]] status_t const single_cleared = main_collection[42].clear();

    // Many keys to one value
    failures += expect(main_collection[{43, 44}].assign("same value").succeeded());

    // Smart references
    [[maybe_unused]] status_t const cleared = main_collection[{43, 44}].clear();
    [[maybe_unused]] status_t const erased = main_collection[{43, 44}].erase();
    [[maybe_unused]] auto const present = main_collection[{43, 44}].present();
    [[maybe_unused]] auto const length = main_collection[{43, 44}].length();
    [[maybe_unused]] auto const value = main_collection[{43, 44}].value();
    [[maybe_unused]] auto const by_array = main_collection[std::array<ustore_key_t, 3> {65, 66, 67}];
    std::array<ustore_key_t, 4> const four_keys {65, 66, 67, 68};
    [[maybe_unused]] auto const by_range = main_collection[strided(four_keys)];
    for ([[maybe_unused]] value_view_t const element : *main_collection[{100, 101}].value()) {}

    // Named collections
    if (database.supports_collections()) {
        blobs_collection_t prefixes = *database.find_or_create("prefixes");
        failures += expect(prefixes.at(42).assign("purpose").succeeded());
        failures += expect(database["articles"]->at(42).assign("of").succeeded());
        failures += expect(database["suffixes"]->at(42).assign("life").succeeded());
    }

    // Reusable memory
    arena_t arena(database);
    [[maybe_unused]] status_t const arena_cleared = main_collection[{43, 44}].on(arena).clear();
    [[maybe_unused]] status_t const arena_erased = main_collection[{43, 44}].on(arena).erase();
    [[maybe_unused]] auto const arena_present = main_collection[{43, 44}].on(arena).present();
    [[maybe_unused]] auto const arena_length = main_collection[{43, 44}].on(arena).length();
    [[maybe_unused]] auto const arena_value = main_collection[{43, 44}].on(arena).value();

    // Counting and streaming keys
    [[maybe_unused]] auto const keys_count = main_collection.size();
    [[maybe_unused]] auto const pairs_count = main_collection.items(100, 200).size();
    auto maybe_stream = main_collection.members(100, 200).keys_begin();
    failures += expect(maybe_stream);
    if (maybe_stream)
        for (keys_stream_t& stream = *maybe_stream; !stream.is_end(); ++stream) {
            [[maybe_unused]] ustore_key_t const key = stream.key();
        }
    [[maybe_unused]] auto const cardinality = main_collection.members(100, 200).size_estimates()->cardinality;

    // Options
    [[maybe_unused]] status_t const unflushed_clear = main_collection[{43, 44}].on(arena).clear(flush_t::dont_flush_k);
    [[maybe_unused]] status_t const unflushed_erase = main_collection[{43, 44}].on(arena).erase(flush_t::dont_flush_k);
    [[maybe_unused]] auto const untracked_present = main_collection[{43, 44}].on(arena).present(watch_t::dont_watch_k);
    [[maybe_unused]] auto const untracked_length = main_collection[{43, 44}].on(arena).length(watch_t::dont_watch_k);
    [[maybe_unused]] auto const untracked_value = main_collection[{43, 44}].on(arena).value(watch_t::dont_watch_k);

    // Sub-documents
    if (database.supports_collections()) {
        docs_collection_t documents = *database.find_or_create<docs_collection_t>("docs");
        failures += expect(documents[56].assign(R"({"hello":"world","answer":42})").succeeded());
        [[maybe_unused]] bool const field_matches = documents[collection_key_field_t {56, "hello"}].value() == "world";
    }

    failures += expect(database.clear().succeeded());
    return failures;
}

int main(int argc, char** argv) {
    std::string_view parent;
    std::array<flag_t, 1> const flags {{directory_flag(parent)}};
    if (argument_status_t const status = parse_test_arguments(argc, argv, flags); status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;

    auto const maybe_directory = test_directory_t::make(parent);
    if (!maybe_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", parent);
        return 1;
    }

    std::size_t total = 0;
    total += report("wrappers.compile", wrappers_compile(maybe_directory->path()));
    return report("total", total) ? 1 : 0;
}
