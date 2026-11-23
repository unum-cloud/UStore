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

#include <array>       // `std::array`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`

#include <fmt/format.h>
#include <yyjson.h>

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
    run_test_case(engine_label_t {"test_docs_range_end_bound"}.c_str(), test_docs_range_end_bound, config);
    run_test_case(engine_label_t {"test_docs_range_descending"}.c_str(), test_docs_range_descending, config);
    run_test_case(engine_label_t {"test_docs_range_remove"}.c_str(), test_docs_range_remove, config);
    run_test_case(engine_label_t {"test_docs_range_measure"}.c_str(), test_docs_range_measure, config);
    return finish_test_run();
}
