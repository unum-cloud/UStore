/**
 * @file api.cpp
 * @author Ashot Vardanian
 * @date 2022-07-06
 *
 * @brief A set of tests implemented using Google Test.
 */

#include <array>
#include <vector>

#include <nlohmann/json.hpp>

#include "ukv/ukv.hpp"

using namespace unum::ukv;
using namespace unum;

#define macro_concat_(prefix, suffix) prefix##suffix
#define macro_concat(prefix, suffix) macro_concat_(prefix, suffix)
#define _ [[maybe_unused]] auto macro_concat(_, __LINE__)

int main(int argc, char** argv) {

    database_t db;
    _ = db.open();

    // Try getting the main collection
    _ = db.main();
    blobs_collection_t main = db.main();

    // Single-element access
    main[42] = "purpose of life";
    main.at(42) = "purpose of life";
    _ = *main[42].value() == "purpose of life";
    _ = main[42].clear();

    // Mapping multiple keys to same values
    main[{43, 44}] = "same value";

    // Operations on smart-references
    _ = main[{43, 44}].clear();
    _ = main[{43, 44}].erase();
    _ = main[{43, 44}].present();
    _ = main[{43, 44}].length();
    _ = main[{43, 44}].value();
    _ = main[std::array<ukv_key_t, 3> {65, 66, 67}];
    _ = main[std::vector<ukv_key_t> {65, 66, 67, 68}];
    for (value_view_t value : *main[{100, 101}].value())
        (void)value;

    // Accessing named collections
    blobs_collection_t prefixes = *db.find_or_create("prefixes");
    prefixes.at(42) = "purpose";
    db["articles"]->at(42) = "of";
    db["suffixes"]->at(42) = "life";

    // Reusable memory
    // This interface not just more performant, but also provides nicer interface:
    //  expected_gt<joined_blobs_t> tapes = main[{100, 101}].on(arena);
    arena_t arena(db);
    _ = main[{43, 44}].on(arena).clear();
    _ = main[{43, 44}].on(arena).erase();
    _ = main[{43, 44}].on(arena).present();
    _ = main[{43, 44}].on(arena).length();
    _ = main[{43, 44}].on(arena).value();

    // Batch-assignment: many keys to many values
    // main[std::array<ukv_key_t, 3> {65, 66, 67}] = std::array {"A", "B", "C"};
    // main[std::array {ckf(prefixes, 65), ckf(66), ckf(67)}] = std::array {"A", "B", "C"};

    // Iterating over collections
    for (ukv_key_t key : main.keys())
        (void)key;
    for (ukv_key_t key : main.keys(100, 200))
        (void)key;

    _ = main.members(100, 200).size_estimates()->cardinality;

    // Supporting options
    _ = main[{43, 44}].on(arena).clear(/*flush:*/ false);
    _ = main[{43, 44}].on(arena).erase(/*flush:*/ false);
    _ = main[{43, 44}].on(arena).present(/*track:*/ false);
    _ = main[{43, 44}].on(arena).length(/*track:*/ false);
    _ = main[{43, 44}].on(arena).value(/*track:*/ false);

    // Working with sub documents
    docs_collection_t docs = *db.find_or_create<docs_collection_t>("docs");
    docs[56] = R"( {"hello": "world", "answer": 42} )"_json.dump().c_str();
    _ = docs[ckf(56, "hello")].value() == "world";

    _ = db.clear();

    return 0;
}