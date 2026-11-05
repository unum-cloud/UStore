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
#include <string_view> // `std::string_view`

#include <fmt/format.h>
#include <yyjson.h>

#include "ustore/ustore.hpp"

#include "abi/fixture.hpp" // `as_text`, `json_equal`, `flat_documents_k`, `check_equalities`, `open_compared`
#include "harness.hpp"     // `suite_exit_code`, `expect`, `expect_eq`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::test::abi;
using namespace unum::ustore::utilities;
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
        ustore_docs_field_type_string_k);
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

/** Every RFC 6902 JSON Patch and RFC 7386 JSON Merge Patch corner case yields
 *  its expected document. */
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

/** Gathers fields of several documents into tables, through typed headers and
 *  type-punned exports. */
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
            field_type_t {"age", ustore_docs_field_type_string_k},
            field_type_t {"person", ustore_docs_field_type_string_k},
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

#pragma region Bounded Ranges

/** Writes a flat document under each integer key in [0, 20) into both @p compared collections. */
std::size_t write_range_documents(compared_collections_t& compared) {
    std::array<ustore_key_t, 20> const keys = keys_of_integers<20>(0);
    std::string_view const document = flat_documents_k[0];
    strided_strings<std::string_view> const documents {{&document, 0}};
    std::size_t failures = 0;
    for (std::size_t side = 0; side != 2; ++side) {
        ustore_outcome_t outcome {};
        ustore_docs_write_t write {};
        write.database = compared.databases[side];
        write.outcome = &outcome;
        write.count = keys.size();
        write.collections = {&compared.collections[side], 0};
        write.keys = {keys.data(), sizeof(ustore_key_t)};
        write.values = documents.view();
        ustore_docs_write(&write);
        failures += expect_success(outcome);
    }
    return failures;
}

/** A documents scan stops before its end key. */
std::size_t docs_range_end_bound(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_docs_k);
    std::size_t failures = expect(compared_status.succeeded());
    if (failures != 0) return failures;
    failures += write_range_documents(compared);
    ustore_key_t const start = key_from_integer(5), end = key_from_integer(9);
    failures += expect_same_scan(ustore_docs_scan, compared, {&start, &end, ustore_scan_ascending_k, 10},
                                 keys_of_integers<4>(5));
    return failures;
}

/** A descending documents scan lists its range largest first. */
std::size_t docs_range_descending(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_docs_k);
    std::size_t failures = expect(compared_status.succeeded());
    if (failures != 0) return failures;
    failures += write_range_documents(compared);
    ustore_key_t const start = key_from_integer(14), end = key_from_integer(4);
    failures += expect_same_scan(ustore_docs_scan, compared, {&start, &end, ustore_scan_descending_k, 3},
                                 keys_of_integers<3>(12, ustore_scan_descending_k));
    return failures;
}

/** A documents range removal removes every document from the start key up to the end key. */
std::size_t docs_range_remove(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_docs_k);
    std::size_t failures = expect(compared_status.succeeded());
    if (failures != 0) return failures;
    failures += write_range_documents(compared);
    ustore_key_t const start = key_from_integer(2), end = key_from_integer(18);
    failures += remove_ranges(ustore_docs_remove, compared, {&start, 1}, {&end, 1});
    std::array<ustore_key_t, 4> const remaining {key_from_integer(0), key_from_integer(1), key_from_integer(18),
                                                 key_from_integer(19)};
    failures += expect_same_scan(ustore_docs_scan, compared, {nullptr, nullptr, ustore_scan_ascending_k, 20},
                                 remaining);
    return failures;
}

/** A documents measure counts the documents of its range with the end excluded. */
std::size_t docs_range_measure(config_t const& config) {
    auto [compared, compared_status] = open_compared(config, ustore_modality_docs_k);
    std::size_t failures = expect(compared_status.succeeded());
    if (failures != 0) return failures;
    failures += write_range_documents(compared);
    ustore_key_t const lower = key_from_integer(5), upper = key_from_integer(9);
    failures += expect_measure_holds(ustore_docs_measure, compared, {&lower, &upper, ustore_scan_ascending_k}, 4);
    failures += expect_measure_holds(ustore_docs_measure, compared, {&upper, nullptr, ustore_scan_descending_k}, 10);
    return failures;
}

#pragma endregion Bounded Ranges

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
    config_t const config {options.engine, maybe_directory->path()};

    report(totals, "database.docs_flat", database_docs_flat, config);
    report(totals, "database.docs_nested_batch", database_docs_nested_batch, config);
    report(totals, "database.docs_modify", database_docs_modify, config);
    report(totals, "database.docs_merge_and_patch", database_docs_merge_and_patch, config);
    report(totals, "database.docs_table", database_docs_table, config);
    report(totals, "docs.range_end_bound", docs_range_end_bound, config);
    report(totals, "docs.range_descending", docs_range_descending, config);
    report(totals, "docs.range_remove", docs_range_remove, config);
    report(totals, "docs.range_measure", docs_range_measure, config);
    return suite_exit_code(totals);
}
