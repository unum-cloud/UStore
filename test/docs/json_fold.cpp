/**
 *  @file test/docs/json_fold.cpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief Checks the docs fold of `ustore/docs/json_fold.hpp` against `ustore_docs_write` on a docs collection of the
 *      engine `--engine` names, the read-modify-write path on the in-memory engine and change records on NVMe, and
 *      checks that composed chains apply like the chains they replace.
 *
 *  Random streams write, upsert, update, insert, patch, merge and remove whole documents and fields of them, with
 *  values of every type, malformed values and patches whose tests fail. Every modification runs through the engine
 *  and through the fold over the base and the pending chain, which must answer the same status and the same bytes.
 *  Chains compose and bases absorb chains at random points, as compaction would. Composition is then checked alone
 *  on random chains and bases, including malformed and empty documents, against applying the chain as it is. On NVMe,
 *  random modifications also run beside the in-memory engine while their records flush, compact and reopen.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`, `std::int64_t`
#include <cstring> // `std::memcpy`, `std::strlen`

#include <algorithm>   // `std::min`
#include <map>         // `std::map`
#include <optional>    // `std::optional`
#include <span>        // `std::span`
#include <string>      // `std::string`
#include <string_view> // `std::string_view`
#include <vector>      // `std::vector`

#include <fmt/format.h> // `fmt::println`

#include "ustore/docs/json_fold.hpp"        // `json_fold`, `json_delta_t`, `encode_deltas`
#include "ustore/ustore.hpp"                // `database_t`, `arena_t`, `strided_strings`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocator`

#include "harness.hpp" // `suite_exit_code`, `expect`, `expect_eq`, `random_t`, `directory_t`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::docs;

namespace unum::ustore::test {

#pragma region Reference

/** Creates a collection named @p name holding documents in @p database. */
expected<ustore_collection_id_t> create_docs_collection(ustore_database_t database, std::string const& name) {
    ustore_collection_id_t created {};
    ustore_outcome_t outcome {};
    ustore_collection_create_t create {};
    create.database = database, create.outcome = &outcome, create.name = bytes_of(std::string_view(name));
    create.modality = ustore_modality_docs_k, create.collection = &created;
    ustore_collection_create(&create);
    if (status_t const status {outcome}; status.failed()) return status;
    return created;
}

/** One document of a docs collection, modified through `ustore_docs_write` and read back as stored bytes, joining
 *  @c transaction when set. */
class reference_t {
    ustore_database_t database_ = nullptr;
    ustore_collection_id_t collection_ {};
    ustore_key_t key_ = key_from_integer(7);

  public:
    ustore_transaction_t transaction = nullptr;

    reference_t(ustore_database_t database, ustore_collection_id_t collection,
                ustore_key_t key = key_from_integer(7)) noexcept
        : database_(database), collection_(collection), key_(key) {}

    /** Stores @p bytes as they are, or removes the document without them. */
    status_t reset(std::optional<std::string> const& bytes) noexcept {
        ustore_outcome_t outcome {};
        if (!bytes) {
            ustore_blobs_remove_t remove {};
            remove.database = database_, remove.outcome = &outcome, remove.transaction = transaction;
            remove.count = 1, remove.collections = {&collection_, 0}, remove.keys = {&key_, 0};
            ustore_blobs_remove(&remove);
            return status_t {outcome};
        }
        std::string_view const text = *bytes;
        strided_strings<std::string_view> const values {{&text, 0}};
        ustore_blobs_write_t write {};
        write.database = database_, write.outcome = &outcome, write.transaction = transaction;
        write.count = 1, write.collections = {&collection_, 0}, write.keys = {&key_, 0};
        write.values = values.view();
        ustore_blobs_write(&write);
        return status_t {outcome};
    }

    /** Writes @p value of @p type into @p field with @p modification, the call a delta stands for. */
    status_t modify(ustore_docs_modification_t modification, std::string_view field, std::string_view value,
                    ustore_docs_field_type_t type) noexcept {
        ustore_outcome_t outcome {};
        strided_strings<std::string_view> const fields {{&field, 0}};
        strided_strings<std::string_view> const values {{&value, 0}};
        ustore_docs_write_t write {};
        write.database = database_, write.outcome = &outcome, write.transaction = transaction;
        write.count = 1, write.collections = {&collection_, 0}, write.keys = {&key_, 0};
        if (!field.empty()) write.fields = fields.view();
        write.values = values.view();
        write.type = type;
        write.modification = modification;
        ustore_docs_write(&write);
        return status_t {outcome};
    }

    /** Removes the document as `ustore_blobs_remove` does. */
    status_t remove() noexcept { return reset(std::nullopt); }

    /** The stored bytes, or nothing for a missing document. */
    std::optional<std::string> read() noexcept {
        auto [arena, arena_status] = arena_t::make();
        if (arena_status.failed()) return std::nullopt;
        ustore_outcome_t outcome {};
        ustore_octet_t const* presences = nullptr;
        ustore_strings_view_t values {};
        ustore_blobs_read_t read {};
        read.database = database_, read.outcome = &outcome;
        read.view = transaction ? ustore_transaction_view(transaction, ustore_watch_none_k) : nullptr;
        read.count = 1, read.collections = {&collection_, 0}, read.keys = {&key_, 0};
        read.presences = &presences, read.values = &values, read.arena = arena;
        ustore_blobs_read(&read);
        if (status_t {outcome}.failed() || !presences || !(presences[0] & 1)) return std::nullopt;
        return std::string(string_of(tape_string(values, 0)));
    }
};

#pragma endregion Reference

#pragma region Random Modifications

/** Writes random JSON over a few keys, so that fields and patches often meet what is there. */
class value_writer_t {
    random_t& random_;

    static constexpr std::string_view keys_k[] = {"a", "b", "c", "0", "1", "x/y", "t~"};
    static constexpr std::string_view scalars_k[] = {
        "0", "1", "-0", "2.5", "1e300", "18446744073709551616", "-7", "\"s\"", "\"\\u00e9\"", "true", "false", "null",
    };

  public:
    explicit value_writer_t(random_t& random) noexcept : random_(random) {}

    std::string_view pick(std::span<std::string_view const> pool) noexcept { return pool[random_.below(pool.size())]; }

    /** A value nested up to @p depth, with @p nulls in objects as often as a merge patch removes members. */
    void value(std::string& out, std::size_t depth, std::size_t nulls = 0) {
        std::uint64_t const kind = depth >= 3 ? 2 : random_.below(4);
        if (kind == 0) {
            out += '{';
            std::size_t const members = random_.below(4);
            for (std::size_t member = 0; member != members; ++member) {
                if (member) out += ',';
                out += '"', out += pick(keys_k), out += "\":";
                if (nulls && random_.below(nulls) == 0) out += "null";
                else value(out, depth + 1, nulls);
            }
            out += '}';
        }
        else if (kind == 1) {
            out += '[';
            std::size_t const elements = random_.below(3);
            for (std::size_t element = 0; element != elements; ++element) {
                if (element) out += ',';
                value(out, depth + 1, nulls);
            }
            out += ']';
        }
        else out += pick(scalars_k);
    }

    /** A whole document, an object at its root most of the time. */
    std::string document() {
        std::string out;
        if (random_.below(8)) {
            out += '{';
            std::size_t const members = random_.below(5);
            for (std::size_t member = 0; member != members; ++member) {
                if (member) out += ',';
                out += '"', out += pick(keys_k), out += "\":";
                value(out, 1);
            }
            out += '}';
        }
        else value(out, 0);
        return out;
    }

    /** An RFC 7386 patch, an object with removals most of the time. */
    std::string merge_patch() {
        std::string out;
        if (random_.below(6)) {
            out += '{';
            std::size_t const members = 1 + random_.below(3);
            for (std::size_t member = 0; member != members; ++member) {
                if (member) out += ',';
                out += '"', out += pick(keys_k), out += "\":";
                value(out, 1, 3);
            }
            out += '}';
        }
        else value(out, 1);
        return out;
    }

    /** An object nested @p depth levels below a document, whose members are removals one time in @p nulls and
     *  nested objects half the rest of the time. */
    std::string nested_object(std::size_t depth, std::size_t nulls) {
        std::string out = "{";
        std::size_t const members = 1 + random_.below(3);
        for (std::size_t member = 0; member != members; ++member) {
            if (member) out += ',';
            out += '"', out += pick(std::span<std::string_view const>(keys_k).first(3)), out += "\":";
            if (nulls && random_.below(nulls) == 0) out += "null";
            else if (depth < 2 && random_.below(2)) out += nested_object(depth + 1, nulls);
            else out += pick(scalars_k);
        }
        out += '}';
        return out;
    }

    /** An RFC 6902 patch of one to three operations over a few paths, whose tests pass now and then. */
    std::string json_patch() {
        static constexpr std::string_view operations_k[] = {"add", "remove", "replace", "move", "copy", "test"};
        static constexpr std::string_view paths_k[] = {"", "/a", "/b", "/a/0", "/0", "/-", "/a/b", "/c", "/x~1y"};
        static constexpr std::string_view small_k[] = {"1", "0", "null", "\"s\"", "{}", "[]", "true"};
        std::string out = "[";
        std::size_t const count = 1 + random_.below(3);
        for (std::size_t index = 0; index != count; ++index) {
            if (index) out += ',';
            std::string_view const operation = pick(operations_k);
            out += "{\"op\":\"", out += operation, out += "\",\"path\":\"", out += pick(paths_k), out += '"';
            if (operation == "move" || operation == "copy") out += ",\"from\":\"", out += pick(paths_k), out += '"';
            if (operation == "add" || operation == "replace" || operation == "test") {
                out += ",\"value\":";
                if (random_.below(2)) out += pick(small_k);
                else value(out, 2);
            }
            out += '}';
        }
        out += ']';
        return out;
    }

    /** Text that is no JSON, or JSON only a lenient reader takes. */
    std::string odd() {
        static constexpr std::string_view odd_k[] = {"{", "", "[1,", "nul", "{\"a\":}", "NaN", "/* c */ {\"a\":1}"};
        return std::string(pick(odd_k));
    }
};

/** The fields modifications address: the whole document, keys, pointers with indices, escapes and a bad escape. */
constexpr std::string_view fields_k[] = {
    "a",    "b",    "c",  "/a", "/b",  "/a/b",  "/a/c", "/a/0", "/a/1",
    "/a/-", "/b/a", "/0", "/1", "x/y", "/x~1y", "/t~0", "/a~2", "/c/a/b",
};

/** One modification a stream makes: what `ustore_docs_write` or `ustore_blobs_remove` receives. */
struct operation_t {
    delta_modification_t modification = delta_modification_t::upsert_k;
    std::string field;
    std::string value;
    ustore_docs_field_type_t type = ustore_docs_field_type_json_k;
};

/** Whether a stream may upsert a whole document with a typed value, which the engine refuses on a malformed one. */
enum class typed_upserts_t : std::uint8_t {
    allowed_k,
    refused_k,
};

/** Draws a modification: whole documents a quarter of the time, a typed value a sixth, an odd value a twelfth. */
operation_t random_operation(random_t& random, value_writer_t& writer, typed_upserts_t typed_upserts) {
    operation_t operation;
    std::uint64_t const roll = random.below(100);
    operation.modification = roll < 22   ? delta_modification_t::upsert_k
                             : roll < 46 ? delta_modification_t::merge_k
                             : roll < 64 ? delta_modification_t::patch_k
                             : roll < 74 ? delta_modification_t::insert_k
                             : roll < 84 ? delta_modification_t::update_k
                             : roll < 95 ? delta_modification_t::write_k
                                         : delta_modification_t::remove_k;
    if (operation.modification == delta_modification_t::remove_k) return operation;
    if (operation.modification == delta_modification_t::write_k) {
        operation.value = random.below(12) ? writer.document() : writer.odd();
        return operation;
    }
    if (random.below(4)) operation.field = std::string(writer.pick(fields_k));
    if (operation.modification == delta_modification_t::patch_k) operation.value = writer.json_patch();
    else if (operation.modification == delta_modification_t::merge_k) operation.value = writer.merge_patch();
    else {
        operation.value = writer.document();
        if (random.below(2)) operation.value.clear(), writer.value(operation.value, 1);
    }
    if (random.below(12) == 0) operation.value = writer.odd();
    else if (random.below(6) == 0 && operation.modification != delta_modification_t::patch_k &&
             (typed_upserts == typed_upserts_t::allowed_k || !operation.field.empty() ||
              operation.modification != delta_modification_t::upsert_k)) {
        // A typed value travels as its bytes to the engine and as printed JSON in the delta
        switch (random.below(5)) {
        case 0: operation.type = ustore_docs_field_type_null_k, operation.value.clear(); break;
        case 1: {
            std::int64_t const integer = std::int64_t(random.below(2000)) - 1000;
            operation.type = ustore_docs_field_type_i64_k;
            operation.value.assign(reinterpret_cast<char const*>(&integer), sizeof(integer));
            break;
        }
        case 2: {
            double const real = double(std::int64_t(random.below(2000)) - 1000) / 8.0 + 0.1;
            operation.type = ustore_docs_field_type_f64_k;
            operation.value.assign(reinterpret_cast<char const*>(&real), sizeof(real));
            break;
        }
        case 3: {
            bool const flag = random.below(2);
            operation.type = ustore_docs_field_type_bool_k;
            operation.value.assign(reinterpret_cast<char const*>(&flag), sizeof(flag));
            break;
        }
        default: operation.type = ustore_docs_field_type_str_k, operation.value = "text \"q\" \xC3\xA9"; break;
        }
    }
    return operation;
}

#pragma endregion Random Modifications

#pragma region Chains

/** One delta record a collection holds: what it does and the bytes it carries. */
struct pending_t {
    delta_kind_t kind = delta_kind_t::change_k;
    std::string bytes;
};

/** The views a fold reads over @p pending. */
std::vector<delta_view_t> views_of(std::vector<pending_t> const& pending) {
    std::vector<delta_view_t> views;
    for (pending_t const& delta : pending)
        views.push_back({delta.kind, {reinterpret_cast<byte_t const*>(delta.bytes.data()), delta.bytes.size()}});
    return views;
}

/** The bytes of @p base as a fold reads them, null for a missing document. */
std::span<byte_t const> base_of(std::optional<std::string> const& base) noexcept {
    if (!base) return {};
    return {reinterpret_cast<byte_t const*>(base->data()), base->size()};
}

/** Encodes @p deltas into one change record, counting a failure into @p failures. */
pending_t change_of(std::span<json_delta_t const> deltas, ustore_arena_t arena, std::size_t& failures) {
    auto [chain, status] = encode_deltas(deltas, arena);
    failures += expect(status.succeeded());
    return {delta_kind_t::change_k, std::string(text_of_bytes(chain))};
}

/** The record @p delta becomes, as the glue logs it: writes and removals as records of their own. */
pending_t pending_of(json_delta_t const& delta, ustore_arena_t arena, std::size_t& failures) {
    if (delta.modification == delta_modification_t::write_k) return {delta_kind_t::write_k, std::string(delta.value)};
    if (delta.modification == delta_modification_t::remove_k) return {delta_kind_t::remove_k, {}};
    return change_of(std::span<json_delta_t const>(&delta, 1), arena, failures);
}

/** The records the encoded @p chain holds, counting a malformed chain into @p failures. */
std::size_t count_records(std::span<byte_t const> chain, std::size_t& failures) {
    std::size_t count = 0;
    status_t const status = for_each_delta(chain, [&](json_delta_t const&) noexcept {
        ++count;
        return status_t {};
    });
    failures += expect(status.succeeded());
    return count;
}

/** What applying a fold answered: a status code, and the bytes on success. */
struct outcome_t {
    status_code_t code = status_code_t::success_k;
    std::string bytes;

    bool operator==(outcome_t const&) const = default;
};

outcome_t outcome_of(expected<std::span<byte_t const>> const& result) {
    if (!result) return {result.status().code, {}};
    return {status_code_t::success_k, std::string(text_of_bytes(*result))};
}

/** Counts a failure when @p actual differs from @p wanted, printing both and the @p context. */
std::size_t expect_outcome(outcome_t const& actual, outcome_t const& wanted, std::string_view context) {
    if (actual == wanted) return 0;
    fmt::println(stderr, "  {}: wanted {} `{}`, got {} `{}`", context, describe(wanted.code), wanted.bytes,
                 describe(actual.code), actual.bytes);
    return 1;
}

#pragma endregion Chains

#pragma region Cases

/** Deltas encode and decode in chains, refuse truncations and unknown modifications, and close as documented. */
std::size_t json_fold_codec() {
    std::size_t failures = 0;
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    json_delta_t const deltas[] = {
        {1, delta_modification_t::merge_k, "/a/b", "{\"x\":null}"},
        {2, delta_modification_t::upsert_k, "", "[1]"},
        {3, delta_modification_t::patch_k, "a", "[]"},
        {~std::uint64_t(0), delta_modification_t::remove_k, "", ""},
    };
    auto [chain, status] = encode_deltas(deltas, arena);
    failures += expect(status.succeeded());
    std::size_t index = 0;
    failures += expect(for_each_delta(chain, [&](json_delta_t const& delta) noexcept -> status_t {
                           json_delta_t const& wanted = deltas[index++];
                           bool const same = delta.stamp == wanted.stamp && delta.modification == wanted.modification &&
                                             delta.field == wanted.field && delta.value == wanted.value;
                           return same ? status_t {} : status_t {status_code_t::internal_k, "differs"};
                       }).succeeded());
    failures += expect_eq(index, std::size_t(4));
    for (std::size_t cut = 1; cut != chain.size(); ++cut) {
        std::size_t boundaries = 0, offset = 0;
        for (json_delta_t const& delta : deltas) boundaries += (offset += encoded_size(delta)) == cut;
        status_t const truncated = for_each_delta(chain.first(cut),
                                                  [](json_delta_t const&) noexcept { return status_t {}; });
        failures += expect(truncated.succeeded() == (boundaries == 1));
    }
    std::string unknown(text_of_bytes(chain.first(encoded_size(deltas[0]))));
    unknown[sizeof(std::uint64_t)] = 9;
    failures += expect_status(
        for_each_delta(bytes_of_text(unknown), [](json_delta_t const&) noexcept { return status_t {}; }),
        status_code_t::malformed_k);

    json_fold const fold;
    auto const closes_chain = [&](json_delta_t const& delta) {
        auto [encoded, encoded_status] = encode_deltas(std::span<json_delta_t const>(&delta, 1), arena);
        return encoded_status.succeeded() && fold.closes({delta_kind_t::change_k, encoded});
    };
    failures += expect(closes_chain({0, delta_modification_t::upsert_k, "", "1"}));
    failures += expect(closes_chain({0, delta_modification_t::write_k, "", "1"}));
    failures += expect(closes_chain({0, delta_modification_t::remove_k, "", ""}));
    failures += expect(!closes_chain({0, delta_modification_t::upsert_k, "a", "1"}));
    failures += expect(!closes_chain({0, delta_modification_t::insert_k, "", "1"}));
    failures += expect(!closes_chain({0, delta_modification_t::merge_k, "", "1"}));
    failures += expect(fold.closes({delta_kind_t::write_k, {}}) && fold.closes({delta_kind_t::remove_k, {}}));
    failures += expect(!fold.closes({delta_kind_t::change_k, bytes_of_text(unknown)}));
    return failures;
}

/** The fold of @p deltas over @p base composed, and the count of records the composition left. */
struct composed_t {
    outcome_t applied;
    outcome_t sequential;
    std::size_t records = 0;
    std::size_t failures = 0;
};

composed_t compose_and_apply(std::optional<std::string> const& base, std::span<json_delta_t const> deltas) {
    composed_t result;
    auto [arena, arena_status] = arena_t::make();
    result.failures += expect(arena_status.succeeded());
    arena_allocator<byte_t> allocator {arena};
    json_fold const fold;
    std::vector<pending_t> pending;
    for (json_delta_t const& delta : deltas) pending.push_back(pending_of(delta, arena, result.failures));
    std::vector<delta_view_t> const views = views_of(pending);
    result.sequential = outcome_of(fold.apply(base_of(base), views, allocator));
    auto [chain, status] = fold.compose(views, allocator);
    result.failures += expect(status.succeeded());
    result.records = count_records(chain, result.failures);
    delta_view_t const composed {delta_kind_t::change_k, chain};
    result.applied = outcome_of(fold.apply(base_of(base), std::span<delta_view_t const>(&composed, 1), allocator));
    return result;
}

/** Each shape composition merges or keeps, on a base where the difference shows. */
std::size_t json_fold_shapes() {
    std::size_t failures = 0;
    std::optional<std::string> const base = R"({"a":{"b":1,"c":[1,2]},"b":2,"c":{"d":3}})";
    struct shape_t {
        char const* name;
        std::vector<json_delta_t> deltas;
        std::size_t records;
        outcome_t wanted;
    };
    using enum delta_modification_t;
    shape_t const shapes[] = {
        {"merges compose",
         {{1, merge_k, "", R"({"a":{"b":null},"z":1})"}, {2, merge_k, "", R"({"a":{"e":2},"b":null})"}},
         1,
         {status_code_t::success_k, R"({"c":{"d":3},"z":1,"a":{"c":[1,2],"e":2}})"}},
        {"a removed member takes no object",
         {{1, merge_k, "", R"({"c":null})"}, {2, merge_k, "", R"({"c":{"e":1}})"}},
         2,
         {status_code_t::success_k, R"({"a":{"b":1,"c":[1,2]},"b":2,"c":{"e":1}})"}},
        {"a scalar member takes no object",
         {{1, merge_k, "", R"({"c":5})"}, {2, merge_k, "", R"({"c":{"e":1}})"}},
         2,
         {status_code_t::success_k, R"({"a":{"b":1,"c":[1,2]},"b":2,"c":{"e":1}})"}},
        {"a repeated key keeps both",
         {{1, merge_k, "", R"({"b":1})"}, {2, merge_k, "", R"({"b":3,"b":4})"}},
         2,
         {status_code_t::success_k, R"({"a":{"b":1,"c":[1,2]},"c":{"d":3},"b":3,"b":4})"}},
        {"upserts of a field past a disjoint one",
         {{1, upsert_k, "/a/b", "5"}, {2, upsert_k, "/c/x", "6"}, {3, upsert_k, "/a/b", "7"}},
         2,
         {status_code_t::success_k, R"({"a":{"b":7,"c":[1,2]},"b":2,"c":{"d":3,"x":6}})"}},
        {"a merge into an upsert",
         {{1, upsert_k, "z", R"({"p":1,"q":2})"}, {2, merge_k, "/z", R"({"p":null,"r":3})"}},
         1,
         {status_code_t::success_k, R"({"a":{"b":1,"c":[1,2]},"b":2,"c":{"d":3},"z":{"q":2,"r":3}})"}},
        {"array siblings stay apart",
         {{1, upsert_k, "/a/c/1", "9"}, {2, insert_k, "/a/c/0", "8"}, {3, upsert_k, "/a/c/1", "7"}},
         3,
         {status_code_t::success_k, R"({"a":{"b":1,"c":[8,7,9]},"b":2,"c":{"d":3}})"}},
        {"a nested field stays apart",
         {{1, upsert_k, "/a/b", "5"}, {2, upsert_k, "/a", "{}"}, {3, upsert_k, "/a/b", "7"}},
         3,
         {status_code_t::success_k, R"({"a":{"b":7},"b":2,"c":{"d":3}})"}},
        {"adjacent patches concatenate",
         {{1, patch_k, "/a", R"([{"op":"add","path":"/e","value":1}])"},
          {2, patch_k, "a", R"([{"op":"test","path":"/e","value":1},{"op":"remove","path":"/b"}])"}},
         1,
         {status_code_t::success_k, R"({"a":{"c":[1,2],"e":1},"b":2,"c":{"d":3}})"}},
        {"patches of two fields stay apart",
         {{1, patch_k, "/a", R"([{"op":"add","path":"/e","value":1}])"},
          {2, patch_k, "/c", R"([{"op":"add","path":"/e","value":2}])"}},
         2,
         {status_code_t::success_k, R"({"a":{"b":1,"c":[1,2],"e":1},"b":2,"c":{"d":3,"e":2}})"}},
        {"a failing patch fails the composed chain",
         {{1, patch_k, "", R"([{"op":"remove","path":"/b"}])"},
          {2, patch_k, "", R"([{"op":"test","path":"/b","value":2}])"}},
         1,
         {status_code_t::invalid_argument_k, {}}},
        {"a whole upsert folds what follows",
         {{1, merge_k, "", R"({"q":1})"}, {2, upsert_k, "", R"({"k":1})"}, {3, upsert_k, "/l", "2"}},
         1,
         {status_code_t::success_k, R"({"k":1,"l":2})"}},
        {"a failure after a closing delta keeps the rest",
         {{1, remove_k, "", ""}, {2, upsert_k, "/l", "2"}},
         2,
         {status_code_t::not_found_k, {}}},
        {"a removal then an insertion",
         {{1, remove_k, "", ""}, {2, insert_k, "", "[1]"}},
         1,
         {status_code_t::success_k, "[1]"}},
        {"a malformed value stays",
         {{1, upsert_k, "/a/b", "{"}, {2, upsert_k, "/a/b", "1"}},
         2,
         {status_code_t::malformed_k, {}}},
    };
    for (shape_t const& shape : shapes) {
        composed_t const result = compose_and_apply(base, shape.deltas);
        std::size_t const before = failures;
        failures += result.failures;
        failures += expect_outcome(result.sequential, shape.wanted, shape.name);
        failures += expect_outcome(result.applied, shape.wanted, shape.name);
        failures += expect_eq(result.records, shape.records);
        if (failures != before) fmt::println(stderr, "  in shape `{}`", shape.name);
    }
    return failures;
}

/** Random streams through the engine and through the fold, with compositions and folded bases at random points. */
std::size_t json_fold_reference(config_t const& config, std::uint64_t seed) {
    std::size_t failures = 0;
    constexpr std::size_t streams_k = 60, steps_k = 160;
    random_t random {seed};
    value_writer_t writer {random};
    json_fold const fold;
    std::size_t checked = 0, refused = 0, compositions = 0, records_before = 0, records_after = 0;
    std::map<status_code_t, std::size_t> refusals;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures) return failures;

    for (std::size_t stream = 0; stream != streams_k && failures == 0; ++stream) {
        auto [collection, create_status] = create_docs_collection(database, fmt::format("stream-{}", stream));
        failures += expect(create_status.succeeded());
        if (failures) return failures;
        reference_t reference {database, collection};

        std::optional<std::string> base;
        std::uint64_t const start = random.below(6);
        if (start == 1) base = "";
        else if (start == 2) base = writer.odd();
        else if (start > 2) base = writer.document();
        failures += expect(reference.reset(base).succeeded());
        std::vector<pending_t> pending;
        std::vector<json_delta_t> history;
        std::vector<std::string> texts;

        for (std::size_t step = 0; step != steps_k && failures == 0; ++step) {
            auto [arena, arena_status] = arena_t::make();
            failures += expect(arena_status.succeeded());
            arena_allocator<byte_t> allocator {arena};

            // A closing upsert ignores the base the engine still parses, so none lands on a malformed document
            std::optional<std::string> const current = reference.read();
            bool const parses = !current || json_parse(std::string_view(*current), arena).status().succeeded();
            operation_t operation = random_operation(random, writer,
                                                     parses ? typed_upserts_t::allowed_k : typed_upserts_t::refused_k);
            // A whole JSON upsert passes through as written, so the glue logs it as a write
            if (operation.modification == delta_modification_t::upsert_k && operation.field.empty() &&
                operation.type == ustore_docs_field_type_json_k)
                operation.modification = delta_modification_t::write_k;

            status_t wanted;
            if (operation.modification == delta_modification_t::remove_k) wanted = reference.remove();
            else if (operation.modification == delta_modification_t::write_k)
                wanted = reference.modify(ustore_docs_modify_upsert_k, {}, operation.value,
                                          ustore_docs_field_type_json_k);
            else
                wanted = reference.modify(static_cast<ustore_docs_modification_t>(operation.modification),
                                          operation.field, operation.value, operation.type);

            // A whole JSON upsert is checked as it is written, and a malformed one never becomes a delta
            if (operation.modification == delta_modification_t::write_k &&
                json_parse(std::string_view(operation.value), arena).status().failed()) {
                failures += expect_status(wanted, status_code_t::malformed_k);
                ++refused;
                continue;
            }
            auto [text, text_status] = modifier_text(std::string_view(operation.value), operation.type, arena);
            failures += expect(text_status.succeeded());
            json_delta_t const delta {stream * steps_k + step, operation.modification, operation.field, text};

            std::vector<pending_t> candidate = pending;
            candidate.push_back(pending_of(delta, arena, failures));
            outcome_t const folded = outcome_of(fold.apply(base_of(base), views_of(candidate), allocator));
            std::optional<std::string> const stored = reference.read();
            outcome_t expected_outcome {wanted.code, {}};
            if (wanted.succeeded()) {
                if (stored) expected_outcome.bytes = *stored;
                else expected_outcome.code = status_code_t::not_found_k;
            }
            ++checked;
            if (expect_outcome(folded, expected_outcome, "fold against the engine")) {
                fmt::println(stderr, "  stream {} step {}: {} `{}` of `{}` over `{}`", stream, step,
                             int(operation.modification), operation.field, operation.value,
                             current.value_or("missing"));
                return failures + 1;
            }
            if (wanted.failed()) {
                ++refused, ++refusals[wanted.code];
                continue;
            }
            pending = std::move(candidate);

            if (random.below(10) == 0) {
                std::vector<delta_view_t> const views = views_of(pending);
                auto [chain, compose_status] = fold.compose(views, allocator);
                failures += expect(compose_status.succeeded());
                delta_view_t const composed {delta_kind_t::change_k, chain};
                outcome_t const applied = outcome_of(
                    fold.apply(base_of(base), std::span<delta_view_t const>(&composed, 1), allocator));
                failures += expect_outcome(applied, expected_outcome, "composed chain against the engine");
                for (pending_t const& delta : pending)
                    records_before += delta.kind != delta_kind_t::change_k
                                          ? 1
                                          : count_records(bytes_of_text(delta.bytes), failures);
                records_after += count_records(chain, failures);
                pending.assign(1, {delta_kind_t::change_k, std::string(text_of_bytes(chain))});
                ++compositions;
            }
            if (random.below(25) == 0) base = stored, pending.clear();
        }
    }
    fmt::println("  {} modifications checked, {} refused, {} compositions took {} records to {}", checked, refused,
                 compositions, records_before, records_after);
    for (auto const& [code, count] : refusals) fmt::println("  {} refused with {}", count, describe(code));
    failures += expect(refusals.size() == 4);
    failures += expect(refused * 20 > checked && refused * 10 < checked * 7);
    failures += expect(records_after * 3 < records_before * 2);
    return failures;
}

/** Composed random chains apply like the chains themselves on random bases, whole and composed in parts. */
std::size_t json_fold_compose(std::uint64_t seed) {
    std::size_t failures = 0;
    random_t random {seed ^ 0x5eed};
    value_writer_t writer {random};
    json_fold const fold;
    constexpr std::string_view few_fields_k[] = {"", "a", "/a", "/a/b", "/a/c", "/b", "/a/0", "/a/1", "/c/a"};
    std::size_t records_before = 0, records_after = 0, failing = 0, rounds = 0;

    for (std::size_t round = 0; round != 3000 && failures == 0; ++round, ++rounds) {
        auto [arena, arena_status] = arena_t::make();
        failures += expect(arena_status.succeeded());
        arena_allocator<byte_t> allocator {arena};

        // Wild rounds draw anything, and clean ones upsert and merge valid values into an object so more chains
        // succeed. Array rounds set and insert siblings of one array, and nested rounds merge objects into objects.
        std::uint64_t const profile = random.below(4);
        bool const clean = profile == 1;
        std::optional<std::string> base;
        std::uint64_t const start = profile ? 5 : random.below(6);
        if (start == 1) base = "";
        else if (start == 2) base = writer.odd();
        else if (start > 2) base = writer.document();
        if (profile == 2) base = R"({"a":[1,2,3],"b":{"c":4}})";
        if (profile == 3) base = writer.nested_object(0, 0);

        std::vector<operation_t> operations;
        std::size_t const count = 1 + random.below(10);
        for (std::size_t index = 0; index != count; ++index) {
            if (profile == 2) {
                constexpr std::string_view array_fields_k[] = {"/a/0", "/a/1", "/a/2", "/a/-", "/b/c", "/b/0"};
                constexpr delta_modification_t array_modifications_k[] = {
                    delta_modification_t::upsert_k, delta_modification_t::upsert_k, delta_modification_t::insert_k,
                    delta_modification_t::merge_k, delta_modification_t::update_k};
                std::string value;
                writer.value(value, 2);
                operations.push_back({array_modifications_k[random.below(5)], std::string(writer.pick(array_fields_k)),
                                      value, ustore_docs_field_type_json_k});
                continue;
            }
            if (profile == 3) {
                operations.push_back({delta_modification_t::merge_k, random.below(4) ? "" : "/a",
                                      writer.nested_object(0, 3), ustore_docs_field_type_json_k});
                continue;
            }
            operation_t operation = random_operation(random, writer, typed_upserts_t::allowed_k);
            if (clean && random.below(4))
                operation.modification = random.below(2) ? delta_modification_t::upsert_k
                                                         : delta_modification_t::merge_k,
                operation.value = writer.merge_patch(), operation.type = ustore_docs_field_type_json_k;
            if (operation.type != ustore_docs_field_type_json_k) {
                auto [text, text_status] = modifier_text(std::string_view(operation.value), operation.type, arena);
                failures += expect(text_status.succeeded());
                operation.value = std::string(text), operation.type = ustore_docs_field_type_json_k;
            }
            if (!operation.field.empty() && random.below(3)) operation.field = std::string(writer.pick(few_fields_k));
            if (operation.modification == delta_modification_t::remove_k && random.below(2))
                operation.modification = delta_modification_t::merge_k, operation.value = writer.merge_patch();
            if (operation.modification == delta_modification_t::write_k && random.below(2))
                operation.modification = delta_modification_t::upsert_k, operation.field = "/a",
                operation.value = writer.merge_patch();
            operations.push_back(std::move(operation));
        }
        std::vector<json_delta_t> deltas;
        for (std::size_t index = 0; index != operations.size(); ++index)
            deltas.push_back(
                {index + 1, operations[index].modification, operations[index].field, operations[index].value});

        // The chain lands in records of random lengths, as write buffers flush them
        std::vector<pending_t> pending;
        for (std::size_t index = 0; index != deltas.size();) {
            std::size_t const length = std::min<std::size_t>(1 + random.below(4), deltas.size() - index);
            std::span<json_delta_t const> const run(deltas.data() + index, length);
            if (length == 1) pending.push_back(pending_of(run.front(), arena, failures));
            else pending.push_back(change_of(run, arena, failures));
            index += length;
        }
        std::vector<delta_view_t> const views = views_of(pending);
        outcome_t const sequential = outcome_of(fold.apply(base_of(base), views, allocator));
        failing += sequential.code != status_code_t::success_k;

        auto [chain, status] = fold.compose(views, allocator);
        failures += expect(status.succeeded());
        delta_view_t const composed {delta_kind_t::change_k, chain};
        outcome_t const applied = outcome_of(
            fold.apply(base_of(base), std::span<delta_view_t const>(&composed, 1), allocator));
        records_before += deltas.size();
        records_after += count_records(chain, failures);
        if (expect_outcome(applied, sequential, "composed chain against the chain")) {
            fmt::println(stderr, "  round {} on base `{}`:", round, base ? *base : std::string("missing"));
            for (json_delta_t const& delta : deltas)
                fmt::println(stderr, "    {} `{}` `{}`", int(delta.modification), delta.field, delta.value);
            return failures + 1;
        }

        // Composing a composed prefix with the rest answers the same again
        std::size_t const split = random.below(views.size() + 1);
        if (split == 0 || split == views.size()) continue;
        auto [prefix, prefix_status] = fold.compose(std::span<delta_view_t const>(views.data(), split), allocator);
        failures += expect(prefix_status.succeeded());
        std::vector<delta_view_t> staged {{delta_kind_t::change_k, prefix}};
        staged.insert(staged.end(), views.begin() + std::ptrdiff_t(split), views.end());
        auto [again, again_status] = fold.compose(staged, allocator);
        failures += expect(again_status.succeeded());
        delta_view_t const recomposed {delta_kind_t::change_k, again};
        failures += expect_outcome(
            outcome_of(fold.apply(base_of(base), std::span<delta_view_t const>(&recomposed, 1), allocator)), sequential,
            "chain composed in parts against the chain");
    }
    fmt::println("  {} chains of {} records composed to {}, {} failing", rounds, records_before, records_after,
                 failing);
    failures += expect(records_after * 4 < records_before * 3);
    failures += expect(failing * 10 > rounds && failing * 10 < rounds * 9);
    return failures;
}

/** Counts a failure when @p reference reads other than @p wanted, printing both and the @p context. */
std::size_t expect_document(reference_t& reference, std::optional<std::string> const& wanted,
                            std::string_view context) {
    std::optional<std::string> const actual = reference.read();
    if (actual == wanted) return 0;
    fmt::println(stderr, "  {}: wanted `{}`, got `{}`", context, wanted.value_or("missing"),
                 actual.value_or("missing"));
    return 1;
}

/** Modifications in a transaction read back folded over the document before it commits, a failing one leaves it as
 *  it was, and the commit lands the rest. */
std::size_t json_fold_own_changes(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures) return failures;
    auto [collection, create_status] = create_docs_collection(database, "own");
    failures += expect(create_status.succeeded());
    if (failures) return failures;
    reference_t head {database, collection};
    failures += expect(head.reset(R"({"a":1})").succeeded());

    auto [transaction, transaction_status] = database.transaction();
    failures += expect(transaction_status.succeeded());
    if (failures) return failures;
    reference_t own {database, collection};
    own.transaction = transaction.transaction();
    constexpr ustore_docs_field_type_t json_k = ustore_docs_field_type_json_k;
    failures += expect(own.modify(ustore_docs_modify_upsert_k, "b", "2", json_k).succeeded());
    failures += expect_document(own, R"({"a":1,"b":2})", "an upsert of a field");
    failures += expect(own.modify(ustore_docs_modify_merge_k, {}, R"({"c":3})", json_k).succeeded());
    failures += expect(
        own.modify(ustore_docs_modify_patch_k, {}, R"([{"op":"remove","path":"/a"}])", json_k).succeeded());
    failures += expect_status(
        own.modify(ustore_docs_modify_patch_k, {}, R"([{"op":"test","path":"/a","value":1}])", json_k),
        status_code_t::invalid_argument_k);
    failures += expect_document(own, R"({"b":2,"c":3})", "a merge and a patch over it");
    failures += expect_document(head, R"({"a":1})", "the head before the commit");
    failures += expect(transaction.commit().has_value());
    failures += expect_document(head, R"({"b":2,"c":3})", "the head after the commit");
    return failures;
}

/** Opens the NVMe engine over @p directory, every collection flushing past 64 KiB, every level twice the one above. */
expected<database_t> open_small_nvme(char const* directory) noexcept {
    constexpr std::size_t write_buffer_bytes_k = 64 * 1024;
    ustore_database_t database = nullptr;
    ustore_outcome_t outcome {};
    ustore_database_open_nvme_t open {};
    open.database = &database;
    open.outcome = &outcome;
    open.directory = {reinterpret_cast<ustore_byte_t const*>(directory), std::strlen(directory)};
    open.collection_write_buffer_max_bytes = write_buffer_bytes_k;
    open.collection_value_max_size = write_buffer_bytes_k / 4;
    open.transaction_max_bytes = write_buffer_bytes_k / 2;
    open.collection_level_enlarge_factor = 2;
    ustore_database_open_nvme(&open);
    if (status_t const status {outcome}; status.failed()) return status;
    return database_t {database};
}

/** Random modifications of a few documents on NVMe, flushed every hundred calls into small levels that merge often,
 *  agree with the in-memory engine call by call, and read the same as the change records reach flushed runs, after a
 *  compaction folds them to the bottom, and after a reopen. */
case_result_t json_fold_maintenance(config_t const& config, std::uint64_t seed) {
    if (config.engine() != "nvme" || !config.directory())
        return case_result_t::skipped("needs the NVMe engine and --directory");
    constexpr std::size_t documents_k = 16, calls_k = 3000, checkpoint_k = 500, flush_k = 100;
    std::size_t failures = 0;
    path_t const home {config.directory(), "maintenance"};
    if (!recreate_directory(home.c_str())) return failures + 1;
    random_t random {seed ^ 0xf01d};
    value_writer_t writer {random};

    auto [memory, memory_status] = open_database<database_t>(config_t {engine_options_t {}, nullptr});
    failures += expect(memory_status.succeeded());
    if (failures) return failures;
    auto [memory_collection, memory_create_status] = create_docs_collection(memory, "maintenance");
    failures += expect(memory_create_status.succeeded());
    if (failures) return failures;
    auto const references_of = [&](ustore_database_t database, ustore_collection_id_t collection) {
        std::vector<reference_t> references;
        for (std::size_t index = 0; index != documents_k; ++index)
            references.emplace_back(database, collection, key_from_integer(index));
        return references;
    };
    std::vector<reference_t> expected_documents = references_of(memory, memory_collection);
    std::vector<reference_t> stored_documents;
    std::size_t landed = 0;
    auto const compare_all = [&](std::string_view context) {
        std::size_t differing = 0;
        for (std::size_t index = 0; index != documents_k; ++index)
            differing += expect_document(stored_documents[index], expected_documents[index].read(), context);
        return differing;
    };

    ustore_collection_id_t drive_collection {};
    {
        auto [drive, drive_status] = open_small_nvme(home.c_str());
        failures += expect(drive_status.succeeded());
        if (failures) return failures;
        auto [created, drive_create_status] = create_docs_collection(drive, "maintenance");
        failures += expect(drive_create_status.succeeded());
        if (failures) return failures;
        drive_collection = created;
        stored_documents = references_of(drive, drive_collection);
        for (std::size_t call = 0; call != calls_k && failures == 0; ++call) {
            std::size_t const index = random.below(documents_k);
            auto [arena, arena_status] = arena_t::make();
            failures += expect(arena_status.succeeded());
            std::optional<std::string> const current = expected_documents[index].read();
            bool const parses = !current || json_parse(std::string_view(*current), arena).status().succeeded();
            operation_t const operation = random_operation(
                random, writer, parses ? typed_upserts_t::allowed_k : typed_upserts_t::refused_k);
            // A whole write lands through a blobs write a third of the time, which takes text that is no JSON as well
            bool const as_blob = operation.modification == delta_modification_t::write_k && random.below(3) == 0;
            auto const apply = [&](reference_t& reference) {
                if (operation.modification == delta_modification_t::remove_k) return reference.remove();
                if (as_blob) return reference.reset(operation.value);
                if (operation.modification == delta_modification_t::write_k)
                    return reference.modify(ustore_docs_modify_upsert_k, {}, operation.value,
                                            ustore_docs_field_type_json_k);
                return reference.modify(static_cast<ustore_docs_modification_t>(operation.modification),
                                        operation.field, operation.value, operation.type);
            };
            status_t const wanted = apply(expected_documents[index]);
            status_t const answered = apply(stored_documents[index]);
            if (answered.code != wanted.code) {
                fmt::println(stderr, "  call {}: wanted {}, got {} for {} `{}` of `{}` over `{}`", call,
                             describe(wanted.code), describe(answered.code), int(operation.modification),
                             operation.field, operation.value, current.value_or("missing"));
                ++failures;
            }
            landed += wanted.succeeded();
            // A compaction flushes the memory runs first, so the records land in runs over bases in older ones
            if (call % flush_k == 0) failures += expect(drive.compact().succeeded());
            if (call % checkpoint_k == 0) failures += compare_all("after a checkpoint");
        }
        fmt::println("  {} of {} modifications landed", landed, calls_k);
        failures += expect(landed * 3 > calls_k);
        failures += compare_all("after every call");
        failures += expect(drive.compact().succeeded());
        failures += compare_all("after a compaction");
    }
    auto [reopened, reopen_status] = open_small_nvme(home.c_str());
    failures += expect(reopen_status.succeeded());
    if (failures) return failures;
    stored_documents = references_of(reopened, drive_collection);
    failures += compare_all("after a reopen");
    return failures;
}

#pragma endregion Cases

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();
    st::expected<directory_t> const directory = directory_t::make(options.directory);
    if (!directory) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }
    config_t const config {options.engine, directory->path()};

    report(totals, "json_fold.codec", json_fold_codec);
    report(totals, "json_fold.shapes", json_fold_shapes);
    report(totals, "json_fold.reference", json_fold_reference, config, std::uint64_t {options.seed.value});
    report(totals, "json_fold.compose", json_fold_compose, std::uint64_t {options.seed.value});
    report(totals, "json_fold.own_changes", json_fold_own_changes, config);
    report(totals, "json_fold.maintenance", json_fold_maintenance, config, std::uint64_t {options.seed.value});

    return suite_exit_code(totals);
}
