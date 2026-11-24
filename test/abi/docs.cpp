/**
 *  @file test/abi/docs.cpp
 *  @author Ash Vardanian
 *  @date November 1, 2026
 *  @brief Unit tests of the documents modality through the C++ wrappers.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int32_t`, `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstdio>  // `stderr`
#include <cstdlib> // `std::free`
#include <cstring> // `std::memcpy`

#include <algorithm>   // `std::reverse`
#include <array>       // `std::array`
#include <span>        // `std::span`
#include <string>      // `std::string`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`
#include <vector>      // `std::vector`

#include <fmt/format.h>
#include <yyjson.h>

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/ustore.hpp"

#include "abi/fixture.hpp" // `as_text`, `json_equal`, `flat_documents_k`, `check_equalities`, `open_compared`
#include "harness.hpp"     // `finish_test_run`, `expect_true`, `expect_equal`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::test::abi;
using namespace unum;

#pragma region JSON

/** A JSON text yyjson wrote for one value, freed with it. */
struct written_json_t {
    char* text = nullptr;

    explicit written_json_t(yyjson_val* value) noexcept : text(value ? yyjson_val_write(value, 0, nullptr) : nullptr) {}
    written_json_t(written_json_t const&) = delete;
    written_json_t& operator=(written_json_t const&) = delete;
    ~written_json_t() noexcept { std::free(text); }
};

/** Whether the document or field @p reference names reads back as JSON equal to @p expected. */
template <typename reference_type_>
bool reads_json(reference_type_&& reference, std::string_view expected) noexcept {
    auto [value, value_status] = reference.value();
    return !value_status.failed() && json_equal(as_text(value), expected);
}

#pragma endregion JSON

#pragma region Documents Modality

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
void test_database_docs_flat(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    auto [collection, collection_status] = database.create<docs_collection_t>("docs");
    if (!expect_true(!collection_status.failed())) return;
    expect_true(!collection[key_from_integer(1)].assign(flat_documents_k[0]).failed());
    expect_true(!collection[key_from_integer(2)].assign(flat_documents_k[1]).failed());
    expect_true(!collection[key_from_integer(3)].assign(flat_documents_k[2]).failed());
    expect_true(reads_json(collection[key_from_integer(1)], flat_documents_k[0]));
    expect_true(reads_json(collection[collection_key_field_t {key_from_integer(2), "person"}], R"("Bob")"));
    expect_true(reads_json(collection[collection_key_field_t {key_from_integer(3), "age"}], "26"));

    auto [person, person_status] = collection[collection_key_field_t {key_from_integer(1), "person"}].value(
        ustore_docs_field_type_string_k);
    expect_true(!person_status.failed());
    expect_equal(std::string_view(person.view()), std::string_view("Alice"));
}

/** Writes nested documents in one batch and reads them back by key and by JSON pointer. */
void test_database_docs_nested_batch(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    auto [collection, collection_status] = database.create<docs_collection_t>("docs");
    if (!expect_true(!collection_status.failed())) return;

    documents_tape_t tape;
    ustore_strings_view_t const values = tape.values();
    tape.assign(nested_documents_k);

    std::array<ustore_key_t, 3> keys {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
    auto reference = collection[keys];
    expect_true(!reference.assign(values).failed());

    expect_true(reads_json(collection[key_from_integer(1)], nested_documents_k[0]));
    expect_true(reads_json(collection[key_from_integer(2)], nested_documents_k[1]));
    expect_true(reads_json(collection[key_from_integer(3)], nested_documents_k[2]));

    expect_true(
        reads_json(collection[collection_key_field_t {key_from_integer(1), "person"}], R"({"name":"Alice","age":24})"));
    expect_true(
        reads_json(collection[collection_key_field_t {key_from_integer(2), "person"}], R"([{"name":"Bob","age":25}])"));
    expect_true(reads_json(collection[collection_key_field_t {key_from_integer(2), "/person/0/name"}], R"("Bob")"));

    check_equalities(reference, tape.views());

    std::array<ustore_key_t, 3> unsorted_keys {key_from_integer(1), key_from_integer(3), key_from_integer(2)};
    auto unsorted_reference = collection[unsorted_keys];
    tape.assign({nested_documents_k[0], nested_documents_k[2], nested_documents_k[1]});
    check_equalities(unsorted_reference, tape.views());

    std::array<ustore_key_t, 3> duplicate_keys {key_from_integer(1), key_from_integer(2), key_from_integer(1)};
    auto duplicate_reference = collection[duplicate_keys];
    tape.assign({nested_documents_k[0], nested_documents_k[1], nested_documents_k[0]});
    check_equalities(duplicate_reference, tape.views());

    std::array<collection_key_field_t, 3> keys_with_fields {
        collection_key_field_t {key_from_integer(1), "person"},
        collection_key_field_t {key_from_integer(2), "/person/0/name"},
        collection_key_field_t {key_from_integer(3), "age"},
    };
    auto fields_reference = collection[keys_with_fields];
    tape.assign({R"({"age":24,"name":"Alice"})", R"("Bob")", "26"});
    check_equalities(fields_reference, tape.views());

    tape.assign({nested_documents_k[0], nested_documents_k[1], R"({"name":"Alice", } "age": 24})"});
    expect_true(reference.assign(values).failed());
}

/** Updates, inserts and upserts whole documents and fields addressed by JSON pointers. */
void test_database_docs_modify(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    auto [collection, collection_status] = database.create<docs_collection_t>("docs");
    if (!expect_true(!collection_status.failed())) return;
    expect_true(!collection[key_from_integer(1)].assign(nested_documents_k[0].data()).failed());
    expect_true(reads_json(collection[key_from_integer(1)], nested_documents_k[0]));

    char const* modifier = R"({"person":{"name":"Charles","age":28}})";
    expect_true(!collection[key_from_integer(1)].update(modifier).failed());
    expect_true(reads_json(collection[key_from_integer(1)], modifier));

    expect_true(!collection[collection_key_field_t {key_from_integer(1), "/person"}]
                     .update(R"({"name":"Alice","age":24})")
                     .failed());
    expect_true(reads_json(collection[key_from_integer(1)], nested_documents_k[0]));

    expect_true(collection[key_from_integer(1)].insert(nested_documents_k[1].data()).failed());
    expect_true(!collection[key_from_integer(2)].insert(nested_documents_k[1].data()).failed());
    expect_true(reads_json(collection[key_from_integer(2)], nested_documents_k[1]));

    expect_true(
        !collection[collection_key_field_t {key_from_integer(2), "/person/0/surname"}].insert(R"("Doe")").failed());
    expect_true(reads_json(collection[key_from_integer(2)], R"({"person":[{"name":"Bob","age":25,"surname":"Doe"}]})"));

    expect_true(!collection[key_from_integer(1)].upsert(nested_documents_k[2].data()).failed());
    expect_true(reads_json(collection[key_from_integer(1)], nested_documents_k[2]));

    expect_true(!collection[collection_key_field_t {key_from_integer(1), "/person"}].upsert(R"("Charles")").failed());
    expect_true(reads_json(collection[key_from_integer(1)], R"({"person":"Charles","age":26})"));

    expect_true(!collection[collection_key_field_t {key_from_integer(1), "/weight"}].upsert("70").failed());
    expect_true(reads_json(collection[key_from_integer(1)], R"({"person":"Charles","age":26,"weight":70})"));
}

/** Writes @p values into @p fields of the documents keyed @p keys in one call with
 *  @p modification. */
ustore_status_t write_documents_once(ustore_database_t database, ustore_collection_id_t collection,
                                     std::span<ustore_key_t const> keys, std::span<std::string_view const> fields,
                                     std::span<std::string_view const> values,
                                     ustore_docs_modification_t modification) noexcept {
    strided_strings<std::string_view> const fields_strings {{fields.data(), sizeof(std::string_view)}};
    strided_strings<std::string_view> const values_strings {{values.data(), sizeof(std::string_view)}};
    ustore_outcome_t outcome {};
    ustore_docs_write_t write {};
    write.database = database;
    write.outcome = &outcome;
    write.count = keys.size();
    write.collections = {&collection, 0};
    write.keys = {keys.data(), sizeof(ustore_key_t)};
    if (!fields.empty()) write.fields = fields_strings.view();
    write.values = values_strings.view();
    write.modification = modification;
    ustore_docs_write(&write);
    return outcome.status;
}

/** One write naming a document twice applies its tasks in order, so field edits of both land and a
 *  second insertion of a key the first one created finds it taken, leaving the key untouched. */
void test_database_docs_repeated_document(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    auto [collection, collection_status] = database.create<docs_collection_t>("docs");
    if (!expect_true(!collection_status.failed())) return;
    expect_true(!collection[key_from_integer(1)].assign(R"({"a":1,"b":1})").failed());

    std::array<ustore_key_t, 2> const repeated_keys {key_from_integer(1), key_from_integer(1)};
    std::array<std::string_view, 2> const fields {"/a", "/b"};
    std::array<std::string_view, 2> const numbers {"2", "3"};
    expect_true(write_documents_once(database.handle(), collection.handle(), repeated_keys, fields, numbers,
                                     ustore_docs_modify_upsert_k) == ustore_status_success_k);
    expect_true(reads_json(collection[key_from_integer(1)], R"({"a":2,"b":3})"));

    std::array<std::string_view, 2> const merges {R"({"a":4})", R"({"b":5})"};
    expect_true(write_documents_once(database.handle(), collection.handle(), repeated_keys, {}, merges,
                                     ustore_docs_modify_merge_k) == ustore_status_success_k);
    expect_true(reads_json(collection[key_from_integer(1)], R"({"a":4,"b":5})"));

    std::array<ustore_key_t, 2> const fresh_keys {key_from_integer(2), key_from_integer(2)};
    std::array<std::string_view, 2> const documents {R"({"x":1})", R"({"x":2})"};
    expect_true(write_documents_once(database.handle(), collection.handle(), fresh_keys, {}, documents,
                                     ustore_docs_modify_insert_k) == ustore_status_already_exists_k);
    expect_true(!collection[key_from_integer(2)].insert(R"({"x":3})").failed());
    expect_true(reads_json(collection[key_from_integer(2)], R"({"x":3})"));
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

/** RFC 7386 JSON Merge Patch corner cases, each a document, a merge patch and
 *  the expected result. */
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
void check_modification_cases(docs_collection_t& collection, document_modification_t modification) {
    bool const patching = modification == document_modification_t::patch_k;
    std::string_view const cases_text = patching ? patch_cases_k : merge_cases_k;
    yyjson_doc* const cases = yyjson_read(cases_text.data(), cases_text.size(), 0);
    expect_true(cases);
    if (!cases) return;

    char const* const modifier_field = patching ? "patch" : "merge";
    yyjson_arr_iter iterator;
    yyjson_arr_iter_init(yyjson_doc_get_root(cases), &iterator);
    while (yyjson_val* const entry = yyjson_arr_iter_next(&iterator)) {
        written_json_t const document {yyjson_obj_get(entry, "doc")};
        written_json_t const modifier {yyjson_obj_get(entry, modifier_field)};
        written_json_t const expected {yyjson_obj_get(entry, "expected")};
        if (!document.text || !modifier.text || !expected.text) continue;

        expect_true(!collection[key_from_integer(1)].assign(static_cast<char const*>(document.text)).failed());
        char const* const modifier_text = static_cast<char const*>(modifier.text);
        expect_true(patching ? !collection[key_from_integer(1)].patch(modifier_text).failed()
                             : !collection[key_from_integer(1)].merge(modifier_text).failed());
        auto [value, value_status] = collection[key_from_integer(1)].value();
        if (!expect_true(!value_status.failed())) continue;
        expect_true(json_equal(as_text(value), expected.text), "{} with {} gave {}",
                    static_cast<char const*>(document.text), modifier_text, as_text(value));
    }
    yyjson_doc_free(cases);
}

/** Every RFC 6902 JSON Patch and RFC 7386 JSON Merge Patch corner case yields
 *  its expected document. */
void test_database_docs_merge_and_patch(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());
    auto [collection, collection_status] = database.create<docs_collection_t>("docs");
    if (!expect_true(!collection_status.failed())) return;

    check_modification_cases(collection, document_modification_t::patch_k);
    check_modification_cases(collection, document_modification_t::merge_k);
}

/** Gathers fields of several documents into tables, through typed headers and
 *  type-punned exports. */
void test_database_docs_table(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    expect_true(!database.clear(ustore_drop_collection_k).failed());

    auto [collection, collection_status] = database.create<docs_collection_t>("docs");
    if (!expect_true(!collection_status.failed())) return;
    char const* const alice = R"({"person":"Alice","age":27,"height":1})";
    char const* const bob = R"({"person":"Bob","age":"27","weight":2})";
    char const* const carl = R"({"person":"Carl","age":24})";
    expect_true(!collection[key_from_integer(1)].assign(alice).failed());
    expect_true(!collection[key_from_integer(2)].assign(bob).failed());
    expect_true(!collection[key_from_integer(3)].assign(carl).failed());
    expect_true(reads_json(collection[key_from_integer(1)], alice));
    expect_true(reads_json(collection[key_from_integer(2)], bob));

    // Every table below lands in one arena, which each gather rewinds for the next
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed())) return;

    // Column names
    {
        auto const [fields, gist_status] = collection[key_from_integer(1)].gist(arena);
        expect_true(!gist_status.failed());
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
        expect_true(has_person);
        expect_true(has_height);
        expect_true(has_age);
        expect_true(!has_weight);
    }

    // Single cell
    {
        auto header = make_table_header().with<std::uint32_t>("age");
        auto const [table, gather_status] = collection[key_from_integer(1)].gather(header, arena);
        expect_true(!gather_status.failed());
        auto ages = table.column<0>();

        expect_equal(ages[0].value, 27u);
        expect_true(!ages[0].converted);
    }

    // Single row
    {
        auto header = make_table_header() //
                          .with<std::uint32_t>("age")
                          .with<std::int32_t>("age")
                          .with<std::string_view>("age");

        auto const [table, gather_status] = collection[key_from_integer(1)].gather(header, arena);
        expect_true(!gather_status.failed());
        auto unsigned_ages = table.column<0>();
        auto signed_ages = table.column<1>();
        auto age_texts = table.column<2>();

        expect_equal(unsigned_ages[0].value, 27u);
        expect_true(!unsigned_ages[0].converted);
        expect_equal(signed_ages[0].value, 27);
        expect_true(signed_ages[0].converted);
        expect_equal(std::string_view(age_texts[0].value.data()), std::string_view("27"));
        expect_true(age_texts[0].converted);
    }

    // Single column
    {
        auto header = make_table_header().with<std::int32_t>("age");
        auto const [table, gather_status] =
            collection[{key_from_integer(1), key_from_integer(2), key_from_integer(3), key_from_integer(123456)}]
                .gather(header, arena);
        expect_true(!gather_status.failed());
        auto ages = table.column<0>();

        expect_equal(ages[0].value, 27);
        expect_equal(ages[1].value, 27);
        expect_true(ages[1].converted);
        expect_equal(ages[2].value, 24);
    }

    // Single strings column
    {
        auto header = make_table_header().with<std::string_view>("age");
        auto const [table, gather_status] =
            collection[{key_from_integer(1), key_from_integer(2), key_from_integer(3), key_from_integer(123456)}]
                .gather(header, arena);
        expect_true(!gather_status.failed());
        auto age_texts = table.column<0>();

        expect_equal(std::string_view(age_texts[0].value.data()), std::string_view("27"));
        expect_true(age_texts[0].converted);
        expect_equal(std::string_view(age_texts[1].value.data()), std::string_view("27"));
        expect_equal(std::string_view(age_texts[2].value.data()), std::string_view("24"));
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

        auto const [table, gather_status] = collection[{key_from_integer(1), key_from_integer(2), key_from_integer(3),
                                                        key_from_integer(123456), key_from_integer(654321)}]
                                                .gather(header, arena);
        expect_true(!gather_status.failed());
        auto ages = table.column<0>();
        auto age_texts = table.column<1>();
        [[maybe_unused]] auto persons = table.column<2>();
        [[maybe_unused]] auto person_numbers = table.column<3>();
        [[maybe_unused]] auto heights = table.column<4>();
        [[maybe_unused]] auto weights = table.column<5>();

        expect_equal(ages[0].value, 27);
        expect_equal(ages[1].value, 27);
        expect_true(ages[1].converted);
        expect_equal(ages[2].value, 24);

        expect_equal(std::string_view(age_texts[0].value.data()), std::string_view("27"));
        expect_true(age_texts[0].converted);
        expect_equal(std::string_view(age_texts[1].value.data()), std::string_view("27"));
        expect_equal(std::string_view(age_texts[2].value.data()), std::string_view("24"));
    }

    // Multiple type-punned columns
    {
        std::array<std::string_view, 6> const fields {"age", "age", "person", "person", "height", "weight"};
        std::array<ustore_docs_field_type_t, 6> const types {
            ustore_docs_field_type_i32_k, ustore_docs_field_type_string_k, ustore_docs_field_type_string_k,
            ustore_docs_field_type_f32_k, ustore_docs_field_type_i32_k,    ustore_docs_field_type_u64_k,
        };
        table_header_view_t const header = make_table_header(fields, types);

        auto const [table, gather_status] = collection[{key_from_integer(1), key_from_integer(2), key_from_integer(3),
                                                        key_from_integer(123456), key_from_integer(654321)}]
                                                .gather(header, arena);
        expect_true(!gather_status.failed());
        auto ages = table.column(0).as<std::int32_t>();
        auto age_texts = table.column(1).as<value_view_t>();
        [[maybe_unused]] auto persons = table.column(2).as<value_view_t>();
        [[maybe_unused]] auto person_numbers = table.column(3).as<float>();
        [[maybe_unused]] auto heights = table.column(4).as<std::int32_t>();
        [[maybe_unused]] auto weights = table.column(5).as<std::uint64_t>();

        expect_equal(ages[0].value, 27);
        expect_equal(ages[1].value, 27);
        expect_true(ages[1].converted);
        expect_equal(ages[2].value, 24);

        expect_equal(std::string_view(age_texts[0].value), std::string_view("27"));
        expect_true(age_texts[0].converted);
        expect_equal(std::string_view(age_texts[1].value), std::string_view("27"));
        expect_equal(std::string_view(age_texts[2].value), std::string_view("24"));
    }
}

/** A gather of more documents than one flight, on an executor of two threads, lands every cell in
 *  its own row: numbers, escaped texts, booleans, collisions, and missing fields and documents. */
void test_docs_gather_in_flights(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    auto [docs, docs_status] = create_collection(database.handle(), "gather_in_flights.docs", ustore_modality_docs_k);
    if (!expect_true(docs_status.succeeded())) return;
    auto [executor, executor_status] = spawn_executor();
    if (!expect_true(executor_status.succeeded())) return;
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(arena_status.succeeded())) return;

    // Three flights of a two-thread executor; every fifth document is missing, every seventh has no
    // "f", every eleventh escapes a quote
    constexpr std::size_t count_k = 3 * 64 * 64 + 960;
    st::vector<ustore_key_t> keys, written_keys;
    st::vector<std::array<char, 96>> texts;
    st::vector<std::string_view> documents;
    if (!expect_true(st::succeeded(keys.resize(count_k)) && st::succeeded(written_keys.resize(count_k)) &&
                     st::succeeded(texts.resize(count_k)) && st::succeeded(documents.resize(count_k))))
        return ustore_executor_free(executor);
    std::size_t written = 0;
    for (std::size_t row = 0; row != count_k; ++row) {
        keys[row] = key_from_integer(row);
        if (row % 5 == 4) continue;
        char const* const quote = row % 11 == 0 ? "\\\"" : "";
        auto const end = row % 7 == 0
                             ? fmt::format_to_n(texts[row].data(), texts[row].size(),
                                                R"({{"i":{},"s":"t{}{}","b":{}}})", row, quote, row, row % 3 == 0)
                             : fmt::format_to_n(texts[row].data(), texts[row].size(),
                                                R"({{"i":{},"s":"t{}{}","f":{}.5,"b":{}}})", row, quote, row, row,
                                                row % 3 == 0);
        written_keys[written] = keys[row];
        documents[written++] = {texts[row].data(), end.size};
    }
    strided_strings<std::string_view> const values {{documents.data(), sizeof(std::string_view)}};
    ustore_outcome_t outcome {};
    ustore_docs_write_t write {
        .database = database.handle(),
        .outcome = &outcome,
        .count = written,
        .collections = {&docs, 0},
        .keys = {written_keys.data(), sizeof(ustore_key_t)},
        .values = values.view(),
    };
    ustore_docs_write(&write);
    expect_success(outcome);

    std::array<std::string_view, 5> const fields {"i", "s", "f", "b", "s"};
    strided_strings<std::string_view> const fields_strings {strided(fields).begin()};
    std::array<ustore_docs_field_type_t, 5> const types {ustore_docs_field_type_i64_k, ustore_docs_field_type_string_k,
                                                         ustore_docs_field_type_f64_k, ustore_docs_field_type_bool_k,
                                                         ustore_docs_field_type_i32_k};
    ustore_docs_column_view_t const* columns = nullptr;
    ustore_docs_gather_t gather {
        .database = database.handle(),
        .executor = executor,
        .outcome = &outcome,
        .count = count_k,
        .collections = {&docs, 0},
        .keys = {keys.data(), sizeof(ustore_key_t)},
        .fields_count = fields.size(),
        .fields = fields_strings.view(),
        .columns = &columns,
        .arena = arena.handle(),
        .outputs_types = types.data(),
    };
    auto const bit = [](ustore_octet_t const* bits, std::size_t row) noexcept {
        return (bits[row / 8] >> (row % 8)) & 1;
    };
    auto const expect_columns = [&] {
        auto const* const numbers = reinterpret_cast<std::int64_t const*>(columns[0].scalars);
        auto const* const halves = reinterpret_cast<double const*>(columns[2].scalars);
        auto const* const flags = reinterpret_cast<bool const*>(columns[3].scalars);
        for (std::size_t row = 0; row != count_k; ++row) {
            bool const present = row % 5 != 4;
            expect_equal(bit(columns[0].validities, row), int {present}, "row {}", row);
            expect_equal(bit(columns[2].validities, row), int {present && row % 7 != 0}, "row {}", row);
            expect_equal(bit(columns[4].collisions, row), int {present}, "row {}", row);
            if (!present) continue;
            expect_equal(numbers[row], static_cast<std::int64_t>(row), "row {}", row);
            expect_equal(flags[row], row % 3 == 0, "row {}", row);
            if (row % 7 != 0) expect_equal(halves[row], static_cast<double>(row) + 0.5, "row {}", row);
            std::array<char, 32> wanted {};
            auto const wanted_end = fmt::format_to_n(wanted.data(), wanted.size(), "t{}{}", row % 11 == 0 ? "\"" : "",
                                                     row);
            std::string_view const text = util::chars_of(util::tape_string(columns[1].strings, row));
            expect_equal(text, std::string_view {wanted.data(), wanted_end.size + 1}, "row {}", row);
        }
    };
    ustore_docs_gather(&gather);
    expect_success(outcome);
    if (!expect_true(columns != nullptr)) return ustore_executor_free(executor);
    expect_columns();

    // A watching transaction gathers every flight as the head does
    auto [transaction, transaction_status] = database.transaction();
    if (transaction_status.succeeded()) {
        gather.view = ustore_transaction_view(transaction.handle(), ustore_watch_keys_k);
        ustore_docs_gather(&gather);
        expect_success(outcome);
        if (expect_true(columns != nullptr)) expect_columns();
    }
    ustore_executor_free(executor);
}

#pragma endregion Documents Modality

#pragma region Bounded Ranges

/** Writes a flat document under each integer key in [0, 20) into both @p compared collections. */
void write_range_documents(compared_collections_t& compared) {
    std::array<ustore_key_t, 20> const keys = keys_of_integers<20>(0);
    std::string_view const document = flat_documents_k[0];
    strided_strings<std::string_view> const documents {{&document, 0}};
    for (std::size_t side = 0; side != 2; ++side) {
        ustore_outcome_t outcome {};
        ustore_docs_write_t write {};
        write.database = compared.databases[side].handle();
        write.outcome = &outcome;
        write.count = keys.size();
        write.collections = {&compared.collections[side], 0};
        write.keys = {keys.data(), sizeof(ustore_key_t)};
        write.values = documents.view();
        ustore_docs_write(&write);
        expect_success(outcome);
    }
}

/** A documents scan stops before its end key. */
void test_docs_range_end_bound(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_docs_k);
    if (!expect_true(!compared_status.failed())) return;
    write_range_documents(compared);
    ustore_key_t const start = key_from_integer(5), end = key_from_integer(9);
    expect_same_scan(ustore_docs_scan, compared, {&start, &end, ustore_scan_ascending_k, 10}, keys_of_integers<4>(5));
}

/** A descending documents scan lists its range largest first. */
void test_docs_range_descending(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_docs_k);
    if (!expect_true(!compared_status.failed())) return;
    write_range_documents(compared);
    ustore_key_t const start = key_from_integer(14), end = key_from_integer(4);
    expect_same_scan(ustore_docs_scan, compared, {&start, &end, ustore_scan_descending_k, 3},
                     keys_of_integers<3>(12, ustore_scan_descending_k));
}

/** A documents range removal removes every document from the start key up to the end key. */
void test_docs_range_remove(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_docs_k);
    if (!expect_true(!compared_status.failed())) return;
    write_range_documents(compared);
    ustore_key_t const start = key_from_integer(2), end = key_from_integer(18);
    remove_ranges(ustore_docs_remove, compared, {&start, 1}, {&end, 1});
    std::array<ustore_key_t, 4> const remaining {key_from_integer(0), key_from_integer(1), key_from_integer(18),
                                                 key_from_integer(19)};
    expect_same_scan(ustore_docs_scan, compared, {nullptr, nullptr, ustore_scan_ascending_k, 20}, remaining);
}

/** A documents measure counts the documents of its range with the end excluded. */
void test_docs_range_measure(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_docs_k);
    if (!expect_true(!compared_status.failed())) return;
    write_range_documents(compared);
    ustore_key_t const lower = key_from_integer(5), upper = key_from_integer(9);
    expect_measure_holds(ustore_docs_measure, compared, {&lower, &upper, ustore_scan_ascending_k}, 4);
    expect_measure_holds(ustore_docs_measure, compared, {&upper, nullptr, ustore_scan_descending_k}, 10);
}

#pragma endregion Bounded Ranges

#pragma region Cursor Batches

/** One row a docs cursor handed out: its key, its whole document, and its "age" field where
 *  valid. */
struct pulled_row_t {
    ustore_key_t key {};
    std::string document;
    std::int64_t age = 0;
    std::uint8_t age_valid = 0;
};

/** The fields every cursor of these cases gathers: the whole document as JSON and "age" as an
 *  i64. */
constexpr std::array<std::uint32_t, 3> rows_field_offsets_k {0, 0, 3};
constexpr std::array<ustore_docs_field_type_t, 2> rows_field_types_k {ustore_docs_field_type_json_k,
                                                                      ustore_docs_field_type_i64_k};

/** Opens a docs cursor over @p collection gathering the whole document and its age, from @p start
 *  in @p direction, through @p view with @p predicate, or null with the failure recorded. */
ustore_docs_cursor_t open_rows_cursor(ustore_database_t database, ustore_executor_t executor,
                                      ustore_collection_id_t collection, ustore_key_t start,
                                      ustore_scan_direction_t direction = ustore_scan_ascending_k,
                                      ustore_view_t view = nullptr, ustore_predicate_t predicate = {},
                                      std::string_view filter = {}) {
    ustore_docs_cursor_t cursor = nullptr;
    ustore_outcome_t outcome {};
    ustore_docs_cursor_init_t init {};
    init.cursor = &cursor, init.database = database, init.executor = executor, init.outcome = &outcome;
    init.view = view, init.collection = collection, init.start_key = start, init.direction = direction;
    init.predicate = predicate, init.filter = bytes_of(filter);
    init.fields_count = rows_field_types_k.size();
    init.fields = util::u32_tape("age", rows_field_offsets_k.data());
    init.columns_types = rows_field_types_k.data();
    ustore_docs_cursor_init(&init);
    expect_success(outcome);
    return cursor;
}

/** Pulls batches of up to @p wanted rows of @p cursor into @p arena, appending their rows to @p
 *  rows, and answers the batches written, checking they hold no more than @p wanted rows and at
 *  least one unless none came. */
std::size_t pull_rows(ustore_database_t database, ustore_executor_t executor, ustore_docs_cursor_t cursor,
                      ustore_arena_t arena, std::size_t wanted, st::vector<pulled_row_t>& rows) {
    ustore_outcome_t outcome {};
    ustore_docs_batch_view_t const* batches = nullptr;
    ustore_size_t count = 0;
    ustore_docs_cursor_next_t next {};
    next.database = database, next.executor = executor, next.outcome = &outcome, next.cursor = cursor;
    next.batches = &batches, next.batches_count = &count, next.arena = arena, next.wanted = wanted;
    ustore_docs_cursor_next(&next);
    if (!expect_success(outcome)) return 0;
    std::size_t pulled = 0;
    for (ustore_docs_batch_view_t const& batch : std::span<ustore_docs_batch_view_t const> {batches, count}) {
        expect_true(batch.count != 0, "a batch holds rows");
        ustore_docs_column_view_t const& documents = batch.columns[0];
        ustore_docs_column_view_t const& ages = batch.columns[1];
        for (std::size_t row = 0; row != batch.count; ++row) {
            pulled_row_t pulled_row;
            pulled_row.key = batch.keys[row];
            pulled_row.document = std::string {util::chars_of(util::tape_string(documents.strings, row))};
            pulled_row.age_valid = (ages.validities[row / 8] >> (row % 8)) & 1u;
            std::memcpy(&pulled_row.age, ages.scalars + row * sizeof(std::int64_t), sizeof(std::int64_t));
            expect_true(st::succeeded(rows.push_back(std::move(pulled_row))));
        }
        pulled += batch.count;
    }
    expect_true(pulled <= wanted, "a pull holds at most what it wanted");
    expect_true(count == 0 || pulled != 0);
    return count;
}

/** Every row a walk handed out, and the most batches one of its pulls wrote. */
struct walked_rows_t {
    st::vector<pulled_row_t> rows;
    std::size_t widest = 0;
};

/** Pulls @p cursor to its end in pulls of @p wanted rows. */
walked_rows_t pull_all_rows(ustore_database_t database, ustore_executor_t executor, ustore_docs_cursor_t cursor,
                            ustore_arena_t arena, std::size_t wanted) {
    walked_rows_t walked;
    for (std::size_t batches = 1; batches;) {
        batches = pull_rows(database, executor, cursor, arena, wanted, walked.rows);
        walked.widest = fu::max_of(walked.widest, batches);
    }
    return walked;
}

/** Writes under every key from @p first up to @p last, exclusive, of @p collection a document whose
 *  age is the key plus @p shift, through @p transaction when given. */
void write_ages(ustore_database_t database, ustore_collection_id_t collection, std::size_t first, std::size_t last,
                std::size_t shift = 0, ustore_transaction_t transaction = nullptr) {
    std::vector<std::string> texts;
    std::vector<ustore_key_t> keys;
    for (std::size_t index = first; index != last; ++index) {
        texts.push_back(fmt::format(R"({{"age":{}}})", index + shift));
        keys.push_back(key_from_integer(index));
    }
    strided_strings<std::string> const documents {{texts.data(), sizeof(std::string)}};
    ustore_outcome_t outcome {};
    ustore_docs_write_t write {};
    write.database = database, write.outcome = &outcome, write.transaction = transaction, write.count = keys.size();
    write.collections = {&collection, 0}, write.keys = {keys.data(), sizeof(ustore_key_t)};
    write.values = documents.view();
    ustore_docs_write(&write);
    expect_success(outcome);
}

/** Removes every key from @p first up to @p last, exclusive, of @p collection, through @p
 *  transaction when given. */
void remove_ages(ustore_database_t database, ustore_collection_id_t collection, std::size_t first, std::size_t last,
                 ustore_transaction_t transaction = nullptr) {
    std::vector<ustore_key_t> keys;
    for (std::size_t index = first; index != last; ++index) keys.push_back(key_from_integer(index));
    ustore_outcome_t outcome {};
    ustore_docs_remove_t remove {};
    remove.database = database, remove.outcome = &outcome, remove.transaction = transaction, remove.count = keys.size();
    remove.collections = {&collection, 0}, remove.keys = {keys.data(), sizeof(ustore_key_t)};
    ustore_docs_remove(&remove);
    expect_success(outcome);
}

/** Flushes @p database, so the writes before it land in a run of their own. */
void flush_runs(ustore_database_t database) {
    ustore_outcome_t outcome {};
    ustore_database_flush_t flush {.database = database, .outcome = &outcome};
    ustore_database_flush(&flush);
    expect_success(outcome);
}

/** Checks @p rows hold exactly the keys @p expected names in its order, each with the age it
 *  maps to. */
template <typename expected_type_>
void expect_rows(st::vector<pulled_row_t> const& rows, std::span<std::size_t const> keys, expected_type_&& age_of) {
    if (!expect_equal(rows.size(), keys.size())) return;
    for (std::size_t index = 0; index != keys.size(); ++index) {
        pulled_row_t const& row = rows[index];
        std::int64_t const age = static_cast<std::int64_t>(age_of(keys[index]));
        expect_true(row.key == key_from_integer(keys[index]), "row {} holds key {}", index, keys[index]);
        expect_true(row.age_valid && row.age == age, "row {} holds age {}", index, age);
        expect_true(json_equal(row.document, fmt::format(R"({{"age":{}}})", age)), "row {} holds its document", index);
    }
}

/** Writes keys 0 to 199 in four flushed runs of fifty, rewrites 45 to 54 across the first fence in
 *  a fifth run, and removes 95 to 104 across the second fence and 160 to 169 in memory runs,
 *  answering the keys left. */
std::vector<std::size_t> write_fenced_runs(ustore_database_t database, ustore_collection_id_t collection) {
    for (std::size_t first = 0; first != 200; first += 50) {
        write_ages(database, collection, first, first + 50);
        flush_runs(database);
    }
    write_ages(database, collection, 45, 55, 1000);
    flush_runs(database);
    remove_ages(database, collection, 95, 105);
    write_ages(database, collection, 160, 170, 2000);
    remove_ages(database, collection, 160, 170);
    std::vector<std::size_t> left;
    for (std::size_t index = 0; index != 200; ++index)
        if (!(index >= 95 && index < 105) && !(index >= 160 && index < 170)) left.push_back(index);
    return left;
}

/** The age a fenced key maps to, the rewritten ones shifted by a thousand. */
std::size_t fenced_age(std::size_t key) noexcept { return key >= 45 && key < 55 ? key + 1000 : key; }

/** A cursor on an executor of four threads splits each pull into batches cut across the fences of
 *  the runs, over rewritten versions and removals straddling them, and walks every key left once,
 *  in order, with its columns; a pull wanting more than is left ends the walk, and the next
 *  answers no batch. */
void test_docs_cursor_batches_cross_run_fences(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    auto [executor, executor_status] = spawn_executor(4);
    if (!expect_true(open_status.succeeded() && arena_status.succeeded() && executor_status.succeeded())) return;
    auto [collection, collection_status] = create_collection(database.handle(), "batches.docs", ustore_modality_docs_k);
    if (!expect_true(collection_status.succeeded())) return;
    std::vector<std::size_t> const left = write_fenced_runs(database.handle(), collection);

    ustore_docs_cursor_t cursor = open_rows_cursor(database.handle(), executor, collection, key_from_integer(0));
    walked_rows_t const walked = pull_all_rows(database.handle(), executor, cursor, arena.handle(), 64);
    expect_rows(walked.rows, left, fenced_age);
    engine_names_t names;
    [[maybe_unused]] unnamed_flag_t const unnamed = read_engine_names(test_run.settings.engine, names);
    if (names.engine == ustore_engine_nvme_k) expect_true(walked.widest > 1, "the tasks cut a pull");
    ustore_docs_cursor_free(cursor);

    // A pull wanting every key left takes them at once and exhausts the walk
    cursor = open_rows_cursor(database.handle(), executor, collection, key_from_integer(0));
    st::vector<pulled_row_t> all;
    expect_true(pull_rows(database.handle(), executor, cursor, arena.handle(), 1000, all) != 0);
    expect_rows(all, left, fenced_age);
    st::vector<pulled_row_t> none;
    expect_equal(pull_rows(database.handle(), executor, cursor, arena.handle(), 1000, none), std::size_t {0});
    ustore_docs_cursor_free(cursor);
    ustore_executor_free(executor);
}

/** A descending cursor walks the fenced runs from the largest key down, every key left once. */
void test_docs_cursor_batches_descend(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    auto [executor, executor_status] = spawn_executor(4);
    if (!expect_true(open_status.succeeded() && arena_status.succeeded() && executor_status.succeeded())) return;
    auto [collection, collection_status] = create_collection(database.handle(), "descending.docs",
                                                             ustore_modality_docs_k);
    if (!expect_true(collection_status.succeeded())) return;
    std::vector<std::size_t> left = write_fenced_runs(database.handle(), collection);
    std::reverse(left.begin(), left.end());
    ustore_docs_cursor_t cursor = open_rows_cursor(database.handle(), executor, collection, largest_key_k,
                                                   ustore_scan_descending_k);
    expect_rows(pull_all_rows(database.handle(), executor, cursor, arena.handle(), 48).rows, left, fenced_age);
    ustore_docs_cursor_free(cursor);
    ustore_executor_free(executor);
}

/** Counts the calls of a predicate admitting even keys, which the executor's threads
 *  ask concurrently. */
struct counted_predicate_t {
    std::size_t calls = 0;
    std::size_t asked = 0;
};

void admit_even_rows(ustore_key_t const* keys, ustore_admission_t* admissions, ustore_size_t count, void* state) {
    auto& counted = *static_cast<counted_predicate_t*>(state);
    fu::standard_atomic_ref<std::size_t> {counted.calls}.fetch_add(1, std::memory_order_relaxed);
    fu::standard_atomic_ref<std::size_t> {counted.asked}.fetch_add(count, std::memory_order_relaxed);
    for (std::size_t index = 0; index != count; ++index)
        admissions[index] = keys[index].uuid.bytes[15] % 2 == 0 ? ustore_admit_k : ustore_reject_k;
}

/** A cheap and an expensive predicate, asked from the tasks of each pull, keep the even keys alone,
 *  every one once. */
void test_docs_cursor_predicates_run_in_tasks(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    auto [executor, executor_status] = spawn_executor(4);
    if (!expect_true(open_status.succeeded() && arena_status.succeeded() && executor_status.succeeded())) return;
    auto [collection, collection_status] = create_collection(database.handle(), "predicates.docs",
                                                             ustore_modality_docs_k);
    if (!expect_true(collection_status.succeeded())) return;
    std::vector<std::size_t> const left = write_fenced_runs(database.handle(), collection);
    std::vector<std::size_t> even;
    for (std::size_t key : left)
        if (key % 2 == 0) even.push_back(key);
    for (ustore_predicate_cost_t const cost : {ustore_predicate_cost_cheap_k, ustore_predicate_cost_expensive_k}) {
        counted_predicate_t counted;
        ustore_docs_cursor_t cursor = open_rows_cursor(database.handle(), executor, collection, key_from_integer(0),
                                                       ustore_scan_ascending_k, nullptr,
                                                       {&admit_even_rows, &counted, cost});
        expect_rows(pull_all_rows(database.handle(), executor, cursor, arena.handle(), 40).rows, even, fenced_age);
        expect_true(counted.calls != 0 && counted.asked >= even.size());
        ustore_docs_cursor_free(cursor);
    }
    ustore_executor_free(executor);
}

/** A cursor through a watching transaction walks the transaction's own writes and removals merged
 *  into the store, its tasks recording what they listed, so a commit landing in the walked range
 *  before the transaction's own conflicts. */
case_result_t test_docs_cursor_transaction_watches_its_tasks(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    auto [executor, executor_status] = spawn_executor(4);
    if (!expect_true(open_status.succeeded() && arena_status.succeeded() && executor_status.succeeded())) return {};
    if (!probed(database.supports_transactions())) {
        ustore_executor_free(executor);
        return case_result_t::skipped(needs_transactions_k);
    }
    auto [collection, collection_status] = create_collection(database.handle(), "watches.docs", ustore_modality_docs_k);
    if (!expect_true(collection_status.succeeded())) return {};
    write_ages(database.handle(), collection, 0, 100);
    flush_runs(database.handle());

    ustore_outcome_t outcome {};
    ustore_transaction_t transaction = nullptr;
    ustore_transaction_init_t begin {.transaction = &transaction, .database = database.handle(), .outcome = &outcome};
    ustore_transaction_init(&begin);
    if (!expect_success(outcome)) return {};
    write_ages(database.handle(), collection, 10, 11, 500, transaction);
    write_ages(database.handle(), collection, 150, 151, 0, transaction);
    remove_ages(database.handle(), collection, 20, 21, transaction);
    ustore_docs_cursor_t cursor = open_rows_cursor(database.handle(), executor, collection, key_from_integer(0),
                                                   ustore_scan_ascending_k,
                                                   ustore_transaction_view(transaction, ustore_watch_keys_k));
    walked_rows_t const walked = pull_all_rows(database.handle(), executor, cursor, arena.handle(), 32);
    std::vector<std::size_t> merged;
    for (std::size_t index = 0; index != 100; ++index)
        if (index != 20) merged.push_back(index);
    merged.push_back(150);
    expect_rows(walked.rows, merged, [](std::size_t key) noexcept { return key == 10 ? key + 500 : key; });
    ustore_docs_cursor_free(cursor);

    write_ages(database.handle(), collection, 30, 31, 7);
    ustore_transaction_commit_t commit {.database = database.handle(), .outcome = &outcome, .transaction = transaction};
    ustore_transaction_commit(&commit);
    expect_true(outcome.status != ustore_status_success_k, "the commit conflicts with a watched key, status {}",
                static_cast<int>(outcome.status));
    ustore_transaction_free(transaction);
    ustore_executor_free(executor);
    return {};
}

/** A docs cursor narrowed by a filter that reads every document hands out, batch by batch, the
 *  documents the filter kept with their keys, the same on the engine and on the reference, over two
 *  flushed runs that overlap and a memory run holding one document's newest version, every pull
 *  holding at least one row until the walk ends. */
void test_docs_cursor_values_follow_their_keys(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_docs_k);
    if (!expect_true(compared_status.succeeded())) return;
    std::array<st::vector<pulled_row_t>, 2> rows;
    for (std::size_t side = 0; side != 2; ++side) {
        ustore_database_t const database = compared.databases[side].handle();
        ustore_collection_id_t const collection = compared.collections[side];
        write_ages(database, collection, 0, 12);
        flush_runs(database);
        write_ages(database, collection, 8, 20);
        flush_runs(database);
        write_ages(database, collection, 13, 14, 100);
        ustore_docs_cursor_t cursor = open_rows_cursor(database, nullptr, collection, key_from_integer(0),
                                                       ustore_scan_ascending_k, nullptr, {}, "?@.age > 7");
        rows[side] = pull_all_rows(database, nullptr, cursor, compared.arenas[side].handle(), 5).rows;
        ustore_docs_cursor_free(cursor);
    }
    std::array<std::size_t, 12> kept {};
    for (std::size_t index = 0; index != kept.size(); ++index) kept[index] = index + 8;
    auto const age_of = [](std::size_t key) noexcept { return key == 13 ? key + 100 : key; };
    expect_rows(rows[0], kept, age_of);
    expect_rows(rows[1], kept, age_of);
}

#pragma endregion Cursor Batches

int main(int argc, char** argv) {
    start_test_run(argc, argv);

    auto [directory, directory_status] = scratch_directory_t::make(test_run.settings.directory);
    if (st::failed(directory_status)) {
        fmt::println(stderr, "Cannot create a unique directory under {}", test_run.settings.directory);
        return 1;
    }
    config_t const config {test_run.settings.engine, directory.path()};

    run_test_case(engine_label_t {"test_database_docs_flat"}.c_str(), test_database_docs_flat, config);
    run_test_case(engine_label_t {"test_database_docs_nested_batch"}.c_str(), test_database_docs_nested_batch, config);
    run_test_case(engine_label_t {"test_database_docs_modify"}.c_str(), test_database_docs_modify, config);
    run_test_case(engine_label_t {"test_database_docs_repeated_document"}.c_str(), test_database_docs_repeated_document,
                  config);
    run_test_case(engine_label_t {"test_database_docs_merge_and_patch"}.c_str(), test_database_docs_merge_and_patch,
                  config);
    run_test_case(engine_label_t {"test_database_docs_table"}.c_str(), test_database_docs_table, config);
    run_test_case(engine_label_t {"test_docs_gather_in_flights"}.c_str(), test_docs_gather_in_flights, config);
    run_test_case(engine_label_t {"test_docs_range_end_bound"}.c_str(), test_docs_range_end_bound, config);
    run_test_case(engine_label_t {"test_docs_range_descending"}.c_str(), test_docs_range_descending, config);
    run_test_case(engine_label_t {"test_docs_range_remove"}.c_str(), test_docs_range_remove, config);
    run_test_case(engine_label_t {"test_docs_range_measure"}.c_str(), test_docs_range_measure, config);
    run_test_case(engine_label_t {"test_docs_cursor_values_follow_their_keys"}.c_str(),
                  test_docs_cursor_values_follow_their_keys, config);
    run_test_case(engine_label_t {"test_docs_cursor_batches_cross_run_fences"}.c_str(),
                  test_docs_cursor_batches_cross_run_fences, config);
    run_test_case(engine_label_t {"test_docs_cursor_batches_descend"}.c_str(), test_docs_cursor_batches_descend,
                  config);
    run_test_case(engine_label_t {"test_docs_cursor_predicates_run_in_tasks"}.c_str(),
                  test_docs_cursor_predicates_run_in_tasks, config);
    run_test_case(engine_label_t {"test_docs_cursor_transaction_watches_its_tasks"}.c_str(),
                  test_docs_cursor_transaction_watches_its_tasks, config);
    return finish_test_run();
}
