/**
 *  @file test/docs/json_fold.cpp
 *  @author Ash Vardanian
 *  @date October 16, 2026
 *  @brief Checks the docs fold of `ustore/docs/json_fold.hpp` against `ustore_docs_write` on a docs collection of the
 *      engine `--engine` names, the read-modify-write path on the in-memory engine and change deltas on NVMe, and
 *      checks that composed chains apply like the chains they replace.
 *
 *  Random streams write, upsert, update, insert, patch, merge and remove whole documents and
 *  fields of them, with values of every type, malformed values and patches whose tests fail.
 *  Every modification runs through the engine and through the fold over the base and the pending
 *  chain, which must answer the same status and the same bytes. Chains compose and bases absorb
 *  chains at random points, as compaction would. Composition is then checked alone on random
 *  chains and bases, including malformed and empty documents, against applying the chain as it
 *  is. On NVMe, random modifications also run beside the in-memory engine while their records
 *  flush, compact and reopen.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`, `std::int64_t`
#include <cstring> // `std::strlen`

#include <map>         // `std::map`
#include <optional>    // `std::optional`
#include <span>        // `std::span`
#include <string>      // `std::string`
#include <string_view> // `std::string_view`
#include <vector>      // `std::vector`

#include <fmt/format.h> // `fmt::println`

#include "ustore/docs/json_fold.hpp"        // `json_fold_t`, `json_delta_t`, `encode_deltas`
#include "ustore/ustore.hpp"                // `database_t`, `arena_t`, `strided_strings`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocator`

#include "harness.hpp" // `run_test_case`, `expect_true`, `expect_status`, `split_mix_generator_t`, `scratch_directory_t`

#if USTORE_OS_WINDOWS_
#include <winsock2.h> // ahead of `<windows.h>`, which would pull in Winsock 1.1
#include <windows.h>  // `VirtualAlloc`, `VirtualFree`
#else
#include <sys/mman.h> // `mmap`, `munmap`
#endif

using namespace unum::ustore::test;
using namespace unum::ustore::docs;
using us::arena_t;
using us::database_t;
using us::strided_strings;

namespace unum::ustore::test {

using util::expected;
using util::status_code_t;
using util::status_t;

#pragma region Reference

/** Creates a collection named @p name holding documents in @p database. */
util::expected<ustore_collection_id_t> create_docs_collection(ustore_database_t database, std::string const& name) {
    ustore_collection_id_t created {};
    ustore_outcome_t outcome {};
    ustore_collection_create_t create {};
    create.database = database, create.outcome = &outcome, create.name = util::bytes_of(std::string_view(name));
    create.modality = ustore_modality_docs_k, create.collection = &created;
    ustore_collection_create(&create);
    if (util::status_t const status = status_of(outcome); status.failed()) return status;
    return created;
}

/** One document of a docs collection, modified through `ustore_docs_write` and read back as stored bytes, joining
 *  @c transaction when set. */
class reference_t {
    ustore_database_t database_ = nullptr;
    ustore_collection_id_t collection_ {};
    ustore_key_t key_ = util::key_from_integer(7);

  public:
    ustore_transaction_t transaction = nullptr;

    reference_t(ustore_database_t database, ustore_collection_id_t collection,
                ustore_key_t key = util::key_from_integer(7)) noexcept
        : database_(database), collection_(collection), key_(key) {}

    /** Stores the JSON document @p bytes as written, or removes the document without them. */
    util::status_t reset(std::optional<std::string> const& bytes) noexcept {
        if (!bytes) return remove();
        return modify(ustore_docs_modify_upsert_k, {}, *bytes, ustore_docs_field_type_json_k);
    }

    /** Writes @p value of @p type into @p field with @p modification, the call a delta stands for. */
    util::status_t modify(ustore_docs_modification_t modification, std::string_view field, std::string_view value,
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
        return status_of(outcome);
    }

    /** Removes the document whole through `ustore_docs_remove`. */
    util::status_t remove() noexcept {
        ustore_outcome_t outcome {};
        ustore_docs_remove_t remove {};
        remove.database = database_, remove.outcome = &outcome, remove.transaction = transaction;
        remove.count = 1, remove.collections = {&collection_, 0}, remove.keys = {&key_, 0};
        ustore_docs_remove(&remove);
        return status_of(outcome);
    }

    /** The stored bytes, read whole as JSON, which pass through as stored, or nothing for a missing document. */
    std::optional<std::string> read() noexcept {
        auto [arena, arena_status] = arena_t::make();
        if (arena_status.failed()) return std::nullopt;
        ustore_outcome_t outcome {};
        ustore_octet_t const* presences = nullptr;
        ustore_strings_view_t values {};
        ustore_docs_read_t read {};
        read.database = database_, read.outcome = &outcome;
        read.view = transaction ? ustore_transaction_view(transaction, ustore_watch_none_k) : nullptr;
        read.count = 1, read.collections = {&collection_, 0}, read.keys = {&key_, 0};
        read.presences = &presences, read.values = &values, read.arena = arena.handle();
        ustore_docs_read(&read);
        if (status_of(outcome).failed() || !presences || !(presences[0] & 1)) return std::nullopt;
        return std::string(ustore::strings_view_t {values, 1}[0]);
    }
};

#pragma endregion Reference

#pragma region Random Modifications

/** Writes random JSON over a few keys, so that fields and patches often meet what is there. */
class value_writer_t {
    split_mix_generator_t& random_;

    static constexpr std::string_view keys_k[] = {"a", "b", "c", "0", "1", "x/y", "t~"};
    static constexpr std::string_view scalars_k[] = {
        "0", "1", "-0", "2.5", "1e300", "18446744073709551616", "-7", "\"s\"", "\"\\u00e9\"", "true", "false", "null",
    };

  public:
    explicit value_writer_t(split_mix_generator_t& random) noexcept : random_(random) {}

    std::string_view pick(std::span<std::string_view const> pool) noexcept { return pool[random_.below(pool.size())]; }

    /** A value nested up to @p depth, with @p nulls in objects as often as a merge patch removes members. */
    void value(std::string& text, std::size_t depth, std::size_t nulls = 0) {
        std::uint64_t const kind = depth >= 3 ? 2 : random_.below(4);
        if (kind == 0) {
            text += '{';
            std::size_t const members = random_.below(4);
            for (std::size_t member = 0; member != members; ++member) {
                if (member) text += ',';
                text += '"', text += pick(keys_k), text += "\":";
                if (nulls && random_.below(nulls) == 0) text += "null";
                else value(text, depth + 1, nulls);
            }
            text += '}';
        }
        else if (kind == 1) {
            text += '[';
            std::size_t const elements = random_.below(3);
            for (std::size_t element = 0; element != elements; ++element) {
                if (element) text += ',';
                value(text, depth + 1, nulls);
            }
            text += ']';
        }
        else text += pick(scalars_k);
    }

    /** A whole document, an object at its root most of the time. */
    std::string document() {
        std::string text;
        if (random_.below(8)) {
            text += '{';
            std::size_t const members = random_.below(5);
            for (std::size_t member = 0; member != members; ++member) {
                if (member) text += ',';
                text += '"', text += pick(keys_k), text += "\":";
                value(text, 1);
            }
            text += '}';
        }
        else value(text, 0);
        return text;
    }

    /** An RFC 7386 patch, an object with removals most of the time. */
    std::string merge_patch() {
        std::string text;
        if (random_.below(6)) {
            text += '{';
            std::size_t const members = 1 + random_.below(3);
            for (std::size_t member = 0; member != members; ++member) {
                if (member) text += ',';
                text += '"', text += pick(keys_k), text += "\":";
                value(text, 1, 3);
            }
            text += '}';
        }
        else value(text, 1);
        return text;
    }

    /** An object nested @p depth levels below a document, whose members are removals one time in @p nulls and
     *  nested objects half the rest of the time. */
    std::string nested_object(std::size_t depth, std::size_t nulls) {
        std::string text = "{";
        std::size_t const members = 1 + random_.below(3);
        for (std::size_t member = 0; member != members; ++member) {
            if (member) text += ',';
            text += '"', text += pick(std::span<std::string_view const>(keys_k).first(3)), text += "\":";
            if (nulls && random_.below(nulls) == 0) text += "null";
            else if (depth < 2 && random_.below(2)) text += nested_object(depth + 1, nulls);
            else text += pick(scalars_k);
        }
        text += '}';
        return text;
    }

    /** An RFC 6902 patch of one to three operations over a few paths, whose tests pass now and then. */
    std::string json_patch() {
        static constexpr std::string_view operations_k[] = {"add", "remove", "replace", "move", "copy", "test"};
        static constexpr std::string_view paths_k[] = {"", "/a", "/b", "/a/0", "/0", "/-", "/a/b", "/c", "/x~1y"};
        static constexpr std::string_view small_k[] = {"1", "0", "null", "\"s\"", "{}", "[]", "true"};
        std::string text = "[";
        std::size_t const count = 1 + random_.below(3);
        for (std::size_t index = 0; index != count; ++index) {
            if (index) text += ',';
            std::string_view const operation = pick(operations_k);
            text += "{\"op\":\"", text += operation, text += "\",\"path\":\"", text += pick(paths_k), text += '"';
            if (operation == "move" || operation == "copy") text += ",\"from\":\"", text += pick(paths_k), text += '"';
            if (operation == "add" || operation == "replace" || operation == "test") {
                text += ",\"value\":";
                if (random_.below(2)) text += pick(small_k);
                else value(text, 2);
            }
            text += '}';
        }
        text += ']';
        return text;
    }

    /** Text that is no strict JSON, NaN and comments included, which every write refuses. */
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
operation_t random_operation(split_mix_generator_t& random, value_writer_t& writer, typed_upserts_t typed_upserts) {
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
        default: operation.type = ustore_docs_field_type_string_k, operation.value = "text \"q\" \xC3\xA9"; break;
        }
    }
    return operation;
}

#pragma endregion Random Modifications

#pragma region Chains

/** One delta a collection holds: its kind and the bytes it carries. */
struct pending_t {
    util::delta_kind_t kind = util::delta_kind_t::change_k;
    std::string bytes;
};

/** The views a fold reads over @p pending. */
std::vector<util::delta_view_t> views_of(std::vector<pending_t> const& pending) {
    std::vector<util::delta_view_t> views;
    for (pending_t const& delta : pending)
        views.push_back({delta.kind, {reinterpret_cast<util::byte_t const*>(delta.bytes.data()), delta.bytes.size()}});
    return views;
}

/** The bytes of @p base as a fold reads them, null for a missing document. */
std::span<util::byte_t const> base_of(std::optional<std::string> const& base) noexcept {
    if (!base) return {};
    return {reinterpret_cast<util::byte_t const*>(base->data()), base->size()};
}

/** Encodes @p deltas into one change delta, failing a check when it cannot. */
pending_t change_of(std::span<json_delta_t const> deltas, ustore_arena_t arena) {
    auto [chain, status] = encode_deltas(deltas, arena);
    expect_true(!status.failed());
    return {util::delta_kind_t::change_k, std::string(util::chars_of(chain))};
}

/** The store delta @p delta becomes, as the glue logs it: writes and removals as deltas of their own. */
pending_t pending_of(json_delta_t const& delta, ustore_arena_t arena) {
    if (delta.modification == delta_modification_t::write_k)
        return {util::delta_kind_t::write_k, std::string(delta.value)};
    if (delta.modification == delta_modification_t::remove_k) return {util::delta_kind_t::remove_k, {}};
    return change_of(std::span<json_delta_t const>(&delta, 1), arena);
}

/** The records the encoded @p chain holds, failing a check on a malformed chain. */
std::size_t count_records(std::span<util::byte_t const> chain) {
    std::size_t count = 0;
    util::status_t const status = for_each_delta(chain, [&](json_delta_t const&) noexcept {
        ++count;
        return util::status_t {};
    });
    expect_true(!status.failed());
    return count;
}

/** What applying a fold answered: a status code, and the bytes on success. */
struct outcome_t {
    util::status_code_t code = util::status_code_t::success_k;
    std::string bytes;

    bool operator==(outcome_t const&) const = default;
};

outcome_t outcome_of(util::expected<std::span<util::byte_t const>> const& result) {
    if (!result) return {result.status().code, {}};
    return {util::status_code_t::success_k, std::string(util::chars_of(*result))};
}

/** Holds when @p actual matches @p wanted ; otherwise fails a check, printing both and
 *  the @p context. */
bool expect_outcome(outcome_t const& actual, outcome_t const& wanted, std::string_view context) {
    return expect_true(actual == wanted, "{}: wanted {} `{}`, got {} `{}`", context, status_name(wanted.code),
                       wanted.bytes, status_name(actual.code), actual.bytes);
}

#if USTORE_OS_WINDOWS_

/** Reserves @p size bytes of address space that fault when touched, or none when it runs out. */
std::span<util::byte_t> reserve_unreadable_bytes(std::size_t size) noexcept {
    void* const reserved = ::VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
    if (!reserved) return {};
    return {static_cast<util::byte_t*>(reserved), size};
}

/** Releases what @c reserve_unreadable_bytes reserved. */
void release_unreadable_bytes(std::span<util::byte_t> reserved) noexcept {
    [[maybe_unused]] BOOL const released = ::VirtualFree(reserved.data(), 0, MEM_RELEASE);
}

#else

/** Reserves @p size bytes of address space that fault when touched, or none when it runs out. */
std::span<util::byte_t> reserve_unreadable_bytes(std::size_t size) noexcept {
    void* const reserved = ::mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reserved == MAP_FAILED) return {};
    return {static_cast<util::byte_t*>(reserved), size};
}

/** Releases what @c reserve_unreadable_bytes reserved. */
void release_unreadable_bytes(std::span<util::byte_t> reserved) noexcept {
    [[maybe_unused]] int const unmapped = ::munmap(reserved.data(), reserved.size());
}

#endif

#pragma endregion Chains

#pragma region Cases

/** Deltas encode and decode in chains, refuse truncations, unknown modifications and unread values
 *  past 4 GiB, and close as documented. */
void test_json_fold_codec() {
    auto [arena, arena_status] = arena_t::make();
    expect_true(!arena_status.failed());
    json_delta_t const deltas[] = {
        {1, delta_modification_t::merge_k, "/a/b", "{\"x\":null}"},
        {2, delta_modification_t::upsert_k, "", "[1]"},
        {3, delta_modification_t::patch_k, "a", "[]"},
        {~std::uint64_t(0), delta_modification_t::remove_k, "", ""},
    };
    auto [chain, status] = encode_deltas(deltas, arena.handle());
    expect_true(!status.failed());
    std::size_t index = 0;
    expect_true(!for_each_delta(chain, [&](json_delta_t const& delta) noexcept -> status_t {
                     json_delta_t const& wanted = deltas[index++];
                     bool const same = delta.stamp == wanted.stamp && delta.modification == wanted.modification &&
                                       delta.field == wanted.field && delta.value == wanted.value;
                     return same ? util::status_t {} : util::status_t {util::status_code_t::internal_k, "differs"};
                 }).failed());
    expect_equal(index, std::size_t(4));
    for (std::size_t prefix_length = 1; prefix_length != chain.size(); ++prefix_length) {
        std::size_t boundaries = 0, offset = 0;
        for (json_delta_t const& delta : deltas) boundaries += (offset += encoded_size(delta)) == prefix_length;
        util::status_t const truncated = for_each_delta(chain.first(prefix_length),
                                                        [](json_delta_t const&) noexcept { return util::status_t {}; });
        expect_true(!truncated.failed() == (boundaries == 1));
    }
    std::string unknown(util::chars_of(chain.first(encoded_size(deltas[0]))));
    unknown[sizeof(std::uint64_t)] = 9;
    expect_status(
        for_each_delta(bytes_of_text(unknown), [](json_delta_t const&) noexcept { return util::status_t {}; }),
        util::status_code_t::malformed_k);

    json_fold_t const fold;
    auto const closes_chain = [&](json_delta_t const& delta) {
        auto [encoded, encoded_status] = encode_deltas(std::span<json_delta_t const>(&delta, 1), arena.handle());
        return !encoded_status.failed() && fold.closes({util::delta_kind_t::change_k, encoded});
    };
    expect_true(closes_chain({0, delta_modification_t::upsert_k, "", "1"}));
    expect_true(closes_chain({0, delta_modification_t::write_k, "", "1"}));
    expect_true(closes_chain({0, delta_modification_t::remove_k, "", ""}));
    expect_true(!closes_chain({0, delta_modification_t::upsert_k, "a", "1"}));
    expect_true(!closes_chain({0, delta_modification_t::insert_k, "", "1"}));
    expect_true(!closes_chain({0, delta_modification_t::merge_k, "", "1"}));
    expect_true(fold.closes({util::delta_kind_t::write_k, {}}) && fold.closes({util::delta_kind_t::remove_k, {}}));
    expect_true(!fold.closes({util::delta_kind_t::change_k, bytes_of_text(unknown)}));

    std::span<util::byte_t> const unreadable = reserve_unreadable_bytes((std::size_t(1) << 32) + 1);
    expect_true(!unreadable.empty());
    json_delta_t const oversized {0, delta_modification_t::upsert_k, "a", util::chars_of(unreadable)};
    expect_status(encode_deltas(std::span<json_delta_t const>(&oversized, 1), arena.handle()),
                  util::status_code_t::capacity_exhausted_k);
    release_unreadable_bytes(unreadable);
}

/** The fold of @p deltas over @p base composed, and the count of records the composition left. */
struct composed_t {
    outcome_t applied;
    outcome_t sequential;
    std::size_t records = 0;
};

composed_t compose_and_apply(std::optional<std::string> const& base, std::span<json_delta_t const> deltas) {
    composed_t result;
    auto [arena, arena_status] = arena_t::make();
    expect_true(!arena_status.failed());
    util::arena_allocator<util::byte_t> allocator {arena.handle()};
    json_fold_t const fold;
    std::vector<pending_t> pending;
    for (json_delta_t const& delta : deltas) pending.push_back(pending_of(delta, arena.handle()));
    std::vector<util::delta_view_t> const views = views_of(pending);
    result.sequential = outcome_of(fold.apply(base_of(base), views, allocator));
    auto [chain, status] = fold.compose(views, allocator);
    expect_true(!status.failed());
    result.records = count_records(chain);
    util::delta_view_t const composed {util::delta_kind_t::change_k, chain};
    result.applied = outcome_of(
        fold.apply(base_of(base), std::span<util::delta_view_t const>(&composed, 1), allocator));
    return result;
}

/** Each shape composition merges or keeps, on a base where the difference shows. */
void test_json_fold_shapes() {
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
         {util::status_code_t::success_k, R"({"c":{"d":3},"z":1,"a":{"c":[1,2],"e":2}})"}},
        {"a removed member takes no object",
         {{1, merge_k, "", R"({"c":null})"}, {2, merge_k, "", R"({"c":{"e":1}})"}},
         2,
         {util::status_code_t::success_k, R"({"a":{"b":1,"c":[1,2]},"b":2,"c":{"e":1}})"}},
        {"a scalar member takes no object",
         {{1, merge_k, "", R"({"c":5})"}, {2, merge_k, "", R"({"c":{"e":1}})"}},
         2,
         {util::status_code_t::success_k, R"({"a":{"b":1,"c":[1,2]},"b":2,"c":{"e":1}})"}},
        {"a repeated key keeps both",
         {{1, merge_k, "", R"({"b":1})"}, {2, merge_k, "", R"({"b":3,"b":4})"}},
         2,
         {util::status_code_t::success_k, R"({"a":{"b":1,"c":[1,2]},"c":{"d":3},"b":3,"b":4})"}},
        {"upserts of a field past a disjoint one",
         {{1, upsert_k, "/a/b", "5"}, {2, upsert_k, "/c/x", "6"}, {3, upsert_k, "/a/b", "7"}},
         2,
         {util::status_code_t::success_k, R"({"a":{"b":7,"c":[1,2]},"b":2,"c":{"d":3,"x":6}})"}},
        {"a merge into an upsert",
         {{1, upsert_k, "z", R"({"p":1,"q":2})"}, {2, merge_k, "/z", R"({"p":null,"r":3})"}},
         1,
         {util::status_code_t::success_k, R"({"a":{"b":1,"c":[1,2]},"b":2,"c":{"d":3},"z":{"q":2,"r":3}})"}},
        {"array siblings stay apart",
         {{1, upsert_k, "/a/c/1", "9"}, {2, insert_k, "/a/c/0", "8"}, {3, upsert_k, "/a/c/1", "7"}},
         3,
         {util::status_code_t::success_k, R"({"a":{"b":1,"c":[8,7,9]},"b":2,"c":{"d":3}})"}},
        {"a nested field stays apart",
         {{1, upsert_k, "/a/b", "5"}, {2, upsert_k, "/a", "{}"}, {3, upsert_k, "/a/b", "7"}},
         3,
         {util::status_code_t::success_k, R"({"a":{"b":7},"b":2,"c":{"d":3}})"}},
        {"adjacent patches concatenate",
         {{1, patch_k, "/a", R"([{"op":"add","path":"/e","value":1}])"},
          {2, patch_k, "a", R"([{"op":"test","path":"/e","value":1},{"op":"remove","path":"/b"}])"}},
         1,
         {util::status_code_t::success_k, R"({"a":{"c":[1,2],"e":1},"b":2,"c":{"d":3}})"}},
        {"patches of two fields stay apart",
         {{1, patch_k, "/a", R"([{"op":"add","path":"/e","value":1}])"},
          {2, patch_k, "/c", R"([{"op":"add","path":"/e","value":2}])"}},
         2,
         {util::status_code_t::success_k, R"({"a":{"b":1,"c":[1,2],"e":1},"b":2,"c":{"d":3,"e":2}})"}},
        {"a failing patch fails the composed chain",
         {{1, patch_k, "", R"([{"op":"remove","path":"/b"}])"},
          {2, patch_k, "", R"([{"op":"test","path":"/b","value":2}])"}},
         1,
         {util::status_code_t::invalid_argument_k, {}}},
        {"a whole upsert folds what follows",
         {{1, merge_k, "", R"({"q":1})"}, {2, upsert_k, "", R"({"k":1})"}, {3, upsert_k, "/l", "2"}},
         1,
         {util::status_code_t::success_k, R"({"k":1,"l":2})"}},
        {"a failure after a closing delta keeps the rest",
         {{1, remove_k, "", ""}, {2, upsert_k, "/l", "2"}},
         2,
         {util::status_code_t::not_found_k, {}}},
        {"a removal then an insertion",
         {{1, remove_k, "", ""}, {2, insert_k, "", "[1]"}},
         1,
         {util::status_code_t::success_k, "[1]"}},
        {"a malformed value stays",
         {{1, upsert_k, "/a/b", "{"}, {2, upsert_k, "/a/b", "1"}},
         2,
         {util::status_code_t::malformed_k, {}}},
    };
    for (shape_t const& shape : shapes) {
        composed_t const result = compose_and_apply(base, shape.deltas);
        expect_outcome(result.sequential, shape.wanted, shape.name);
        expect_outcome(result.applied, shape.wanted, shape.name);
        expect_equal(result.records, shape.records, "in shape `{}`", shape.name);
    }
}

/** Random streams through the engine and through the fold, with compositions and folded bases at random points. */
void test_json_fold_reference(config_t const& config) {
    constexpr std::size_t streams_k = 60, steps_k = 160;
    split_mix_generator_t random {case_stream_key()};
    value_writer_t writer {random};
    json_fold_t const fold;
    std::size_t checked = 0, refused = 0, compositions = 0, records_before = 0, records_after = 0;
    std::map<util::status_code_t, std::size_t> refusals;
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;

    for (std::size_t stream = 0; stream != streams_k; ++stream) {
        auto [collection, create_status] = create_docs_collection(database.handle(), fmt::format("stream-{}", stream));
        if (!expect_true(!create_status.failed())) return;
        reference_t reference {database.handle(), collection};

        std::optional<std::string> base;
        // Only the docs verbs reach a docs collection, so the engine's bases are documents or none, while empty and
        // malformed bases stay with the composition rounds below
        if (random.below(6) > 2) base = writer.document();
        if (!expect_true(!reference.reset(base).failed())) return;
        std::vector<pending_t> pending;
        std::vector<json_delta_t> history;
        std::vector<std::string> texts;

        for (std::size_t step = 0; step != steps_k; ++step) {
            auto [arena, arena_status] = arena_t::make();
            if (!expect_true(!arena_status.failed())) return;
            util::arena_allocator<util::byte_t> allocator {arena.handle()};

            // A closing upsert ignores the base the engine still parses, so none lands on a malformed document
            std::optional<std::string> const current = reference.read();
            bool const parses = !current || !json_parse(std::string_view(*current), arena.handle()).status().failed();
            operation_t operation = random_operation(random, writer,
                                                     parses ? typed_upserts_t::allowed_k : typed_upserts_t::refused_k);
            // A whole JSON upsert passes through as written, so the glue logs it as a write
            if (operation.modification == delta_modification_t::upsert_k && operation.field.empty() &&
                operation.type == ustore_docs_field_type_json_k)
                operation.modification = delta_modification_t::write_k;

            util::status_t wanted;
            if (operation.modification == delta_modification_t::remove_k) wanted = reference.remove();
            else if (operation.modification == delta_modification_t::write_k)
                wanted = reference.modify(ustore_docs_modify_upsert_k, {}, operation.value,
                                          ustore_docs_field_type_json_k);
            else
                wanted = reference.modify(static_cast<ustore_docs_modification_t>(operation.modification),
                                          operation.field, operation.value, operation.type);

            // A whole JSON upsert is checked as it is written, and a malformed one never becomes a delta
            if (operation.modification == delta_modification_t::write_k &&
                json_parse(std::string_view(operation.value), arena.handle()).status().failed()) {
                expect_status(wanted, util::status_code_t::malformed_k);
                ++refused;
                continue;
            }
            auto [text, text_status] = modifier_text(std::string_view(operation.value), operation.type, arena.handle());
            if (!expect_true(!text_status.failed())) return;
            json_delta_t const delta {stream * steps_k + step, operation.modification, operation.field, text};

            std::vector<pending_t> candidate = pending;
            candidate.push_back(pending_of(delta, arena.handle()));
            outcome_t const folded = outcome_of(fold.apply(base_of(base), views_of(candidate), allocator));
            std::optional<std::string> const stored = reference.read();
            outcome_t expected_outcome {wanted.code, {}};
            if (!wanted.failed()) {
                if (stored) expected_outcome.bytes = *stored;
                else expected_outcome.code = util::status_code_t::not_found_k;
            }
            ++checked;
            if (!expect_outcome(folded, expected_outcome, "fold against the engine")) {
                fmt::println(stderr, "    stream {} step {}: {} `{}` of `{}` over `{}`", stream, step,
                             int(operation.modification), operation.field, operation.value,
                             current.value_or("missing"));
                return;
            }
            if (wanted.failed()) {
                ++refused, ++refusals[wanted.code];
                continue;
            }
            pending = std::move(candidate);

            if (random.below(10) == 0) {
                std::vector<util::delta_view_t> const views = views_of(pending);
                auto [chain, compose_status] = fold.compose(views, allocator);
                if (!expect_true(!compose_status.failed())) return;
                util::delta_view_t const composed {util::delta_kind_t::change_k, chain};
                outcome_t const applied = outcome_of(
                    fold.apply(base_of(base), std::span<util::delta_view_t const>(&composed, 1), allocator));
                if (!expect_outcome(applied, expected_outcome, "composed chain against the engine")) return;
                for (pending_t const& delta : pending)
                    records_before += delta.kind != util::delta_kind_t::change_k
                                          ? 1
                                          : count_records(bytes_of_text(delta.bytes));
                records_after += count_records(chain);
                pending.assign(1, {util::delta_kind_t::change_k, std::string(util::chars_of(chain))});
                ++compositions;
            }
            if (random.below(25) == 0) base = stored, pending.clear();
        }
    }
    fmt::println("  {} modifications checked, {} refused, {} compositions took {} records to {}", checked, refused,
                 compositions, records_before, records_after);
    for (auto const& [code, count] : refusals) fmt::println(stderr, "  {} refused with {}", count, status_name(code));
    expect_true(refusals.size() == 4);
    expect_true(refused * 20 > checked && refused * 10 < checked * 7);
    expect_true(records_after * 3 < records_before * 2);
}

/** Composed random chains apply like the chains themselves on random bases, whole and composed in parts. */
void test_json_fold_compose() {
    split_mix_generator_t random {case_stream_key()};
    value_writer_t writer {random};
    json_fold_t const fold;
    constexpr std::string_view few_fields_k[] = {"", "a", "/a", "/a/b", "/a/c", "/b", "/a/0", "/a/1", "/c/a"};
    std::size_t records_before = 0, records_after = 0, failing = 0, rounds = 0;

    for (std::size_t round = 0; round != 3000; ++round, ++rounds) {
        auto [arena, arena_status] = arena_t::make();
        if (!expect_true(!arena_status.failed())) return;
        util::arena_allocator<util::byte_t> allocator {arena.handle()};

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
                auto [text, text_status] = modifier_text(std::string_view(operation.value), operation.type,
                                                         arena.handle());
                if (!expect_true(!text_status.failed())) return;
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

        // The chain lands in records of random lengths, as flushes of memory runs write them
        std::vector<pending_t> pending;
        for (std::size_t index = 0; index != deltas.size();) {
            std::size_t const length = fu::min_of<std::size_t>(1 + random.below(4), deltas.size() - index);
            std::span<json_delta_t const> const run(deltas.data() + index, length);
            if (length == 1) pending.push_back(pending_of(run.front(), arena.handle()));
            else pending.push_back(change_of(run, arena.handle()));
            index += length;
        }
        std::vector<util::delta_view_t> const views = views_of(pending);
        outcome_t const sequential = outcome_of(fold.apply(base_of(base), views, allocator));
        failing += sequential.code != util::status_code_t::success_k;

        auto [chain, status] = fold.compose(views, allocator);
        if (!expect_true(!status.failed())) return;
        util::delta_view_t const composed {util::delta_kind_t::change_k, chain};
        outcome_t const applied = outcome_of(
            fold.apply(base_of(base), std::span<util::delta_view_t const>(&composed, 1), allocator));
        records_before += deltas.size();
        records_after += count_records(chain);
        if (!expect_outcome(applied, sequential, "composed chain against the chain")) {
            fmt::println(stderr, "    round {} on base `{}`:", round, base ? *base : std::string("missing"));
            for (json_delta_t const& delta : deltas)
                fmt::println(stderr, "      {} `{}` `{}`", int(delta.modification), delta.field, delta.value);
            return;
        }

        // Composing a composed prefix with the rest answers the same again
        std::size_t const split = random.below(views.size() + 1);
        if (split == 0 || split == views.size()) continue;
        auto [prefix, prefix_status] = fold.compose(std::span<util::delta_view_t const>(views.data(), split),
                                                    allocator);
        if (!expect_true(!prefix_status.failed())) return;
        std::vector<util::delta_view_t> resumed {{util::delta_kind_t::change_k, prefix}};
        resumed.insert(resumed.end(), views.begin() + std::ptrdiff_t(split), views.end());
        auto [again, again_status] = fold.compose(resumed, allocator);
        if (!expect_true(!again_status.failed())) return;
        util::delta_view_t const recomposed {util::delta_kind_t::change_k, again};
        if (!expect_outcome(
                outcome_of(fold.apply(base_of(base), std::span<util::delta_view_t const>(&recomposed, 1), allocator)),
                sequential, "chain composed in parts against the chain"))
            return;
    }
    fmt::println("  {} chains of {} records composed to {}, {} failing", rounds, records_before, records_after,
                 failing);
    expect_true(records_after * 4 < records_before * 3);
    expect_true(failing * 10 > rounds && failing * 10 < rounds * 9);
}

/** Holds when @p reference reads @p wanted ; otherwise fails a check, printing both and
 *  the @p context. */
bool expect_document(reference_t& reference, std::optional<std::string> const& wanted, std::string_view context) {
    std::optional<std::string> const actual = reference.read();
    return expect_true(actual == wanted, "{}: wanted `{}`, got `{}`", context, wanted.value_or("missing"),
                       actual.value_or("missing"));
}

/** Modifications in a transaction read back folded over the document before it commits, a failing one leaves it as
 *  it was, and the commit lands the rest. */
case_result_t test_json_fold_own_changes(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return {};
    if (!probed(database.supports_transactions())) return case_result_t::skipped(needs_transactions_k);
    auto [collection, create_status] = create_docs_collection(database.handle(), "own");
    if (!expect_true(!create_status.failed())) return {};
    reference_t head {database.handle(), collection};
    expect_true(!head.reset(R"({"a":1})").failed());

    auto [transaction, transaction_status] = database.transaction();
    if (!expect_true(!transaction_status.failed())) return {};
    reference_t uncommitted {database.handle(), collection};
    uncommitted.transaction = transaction.handle();
    constexpr ustore_docs_field_type_t json_k = ustore_docs_field_type_json_k;
    expect_true(!uncommitted.modify(ustore_docs_modify_upsert_k, "b", "2", json_k).failed());
    expect_document(uncommitted, R"({"a":1,"b":2})", "an upsert of a field");
    expect_true(!uncommitted.modify(ustore_docs_modify_merge_k, {}, R"({"c":3})", json_k).failed());
    expect_true(
        !uncommitted.modify(ustore_docs_modify_patch_k, {}, R"([{"op":"remove","path":"/a"}])", json_k).failed());
    expect_status(
        uncommitted.modify(ustore_docs_modify_patch_k, {}, R"([{"op":"test","path":"/a","value":1}])", json_k),
        util::status_code_t::invalid_argument_k);
    expect_document(uncommitted, R"({"b":2,"c":3})", "a merge and a patch over it");
    expect_document(head, R"({"a":1})", "the head before the commit");
    expect_true(!transaction.commit().status.failed());
    expect_document(head, R"({"b":2,"c":3})", "the head after the commit");
    return {};
}

/** Opens the NVMe engine over a manifest in @p directory, every collection flushing past 64 KiB,
 *  every level twice the one above. */
util::expected<database_t> open_small_nvme(char const* directory) noexcept {
    constexpr std::size_t memory_runs_bytes_k = 64 * 1024;
    ustore_database_t database = nullptr;
    ustore_outcome_t outcome {};
    ustore_database_open_nvme_t open {};
    open.database = &database;
    open.outcome = &outcome;
    std::string const manifest = fmt::format("{}/{}", directory, "docs.ustore.json");
    open.directory = {reinterpret_cast<ustore_byte_t const*>(manifest.c_str()), std::strlen(manifest.c_str())};
    open.collection_memory_runs_max_bytes = memory_runs_bytes_k;
    open.collection_value_max_size = memory_runs_bytes_k / 4;
    open.transaction_max_bytes = memory_runs_bytes_k / 2;
    open.collection_level_enlarge_factor = 2;
    ustore_database_open_nvme(&open);
    if (util::status_t const status = status_of(outcome); status.failed()) return status;
    return database_t {database};
}

/** Random modifications of a few documents on NVMe, flushed every hundred calls into small levels that merge often,
 *  agree with the in-memory engine call by call, and read the same as the change deltas reach flushed runs, after a
 *  compaction folds them to the bottom, and after a reopen. */
case_result_t test_json_fold_maintenance(config_t const& config) {
    if (config.engine() != "nvme" || !config.directory())
        return case_result_t::skipped("needs the NVMe engine and --directory");
    constexpr std::size_t documents_k = 16, calls_k = 3000, checkpoint_k = 500, flush_k = 100;
    std::string const home = fmt::format("{}/{}", config.directory(), "maintenance");
    if (!expect_true(scratch_directory_t::recreate(home.c_str()))) return {};
    split_mix_generator_t random {case_stream_key()};
    value_writer_t writer {random};

    auto [memory, memory_status] = open_database<database_t>(config_t {engine_options_t {}, nullptr});
    if (!expect_true(!memory_status.failed())) return {};
    auto [memory_collection, memory_create_status] = create_docs_collection(memory.handle(), "maintenance");
    if (!expect_true(!memory_create_status.failed())) return {};
    auto const references_of = [&](ustore_database_t database, ustore_collection_id_t collection) {
        std::vector<reference_t> references;
        for (std::size_t index = 0; index != documents_k; ++index)
            references.emplace_back(database, collection, util::key_from_integer(index));
        return references;
    };
    std::vector<reference_t> expected_documents = references_of(memory.handle(), memory_collection);
    std::vector<reference_t> stored_documents;
    std::size_t landed = 0;
    auto const compare_all = [&](std::string_view context) {
        bool same = true;
        for (std::size_t index = 0; index != documents_k; ++index)
            same = expect_document(stored_documents[index], expected_documents[index].read(), context) && same;
        return same;
    };

    ustore_collection_id_t drive_collection {};
    {
        auto [drive, drive_status] = open_small_nvme(home.c_str());
        if (!expect_true(!drive_status.failed())) return {};
        auto [created, drive_create_status] = create_docs_collection(drive.handle(), "maintenance");
        if (!expect_true(!drive_create_status.failed())) return {};
        drive_collection = created;
        stored_documents = references_of(drive.handle(), drive_collection);
        for (std::size_t call = 0; call != calls_k; ++call) {
            std::size_t const index = random.below(documents_k);
            auto [arena, arena_status] = arena_t::make();
            if (!expect_true(!arena_status.failed())) return {};
            std::optional<std::string> const current = expected_documents[index].read();
            bool const parses = !current || !json_parse(std::string_view(*current), arena.handle()).status().failed();
            operation_t const operation = random_operation(
                random, writer, parses ? typed_upserts_t::allowed_k : typed_upserts_t::refused_k);
            auto const apply = [&](reference_t& reference) {
                if (operation.modification == delta_modification_t::remove_k) return reference.remove();
                if (operation.modification == delta_modification_t::write_k)
                    return reference.modify(ustore_docs_modify_upsert_k, {}, operation.value,
                                            ustore_docs_field_type_json_k);
                return reference.modify(static_cast<ustore_docs_modification_t>(operation.modification),
                                        operation.field, operation.value, operation.type);
            };
            util::status_t const wanted = apply(expected_documents[index]);
            util::status_t const answered = apply(stored_documents[index]);
            if (!expect_true(answered.code == wanted.code, "call {}: wanted {}, got {} for {} `{}` of `{}` over `{}`",
                             call, status_name(wanted.code), status_name(answered.code), int(operation.modification),
                             operation.field, operation.value, current.value_or("missing")))
                return {};
            landed += !wanted.failed();
            // A compaction flushes the memory runs first, so the records land in runs over bases in older ones
            if (call % flush_k == 0 && !expect_true(!drive.compact().failed())) return {};
            if (call % checkpoint_k == 0 && !compare_all("after a checkpoint")) return {};
        }
        fmt::println("  {} of {} modifications landed", landed, calls_k);
        expect_true(landed * 3 > calls_k);
        compare_all("after every call");
        expect_true(!drive.compact().failed());
        compare_all("after a compaction");
    }
    auto [reopened, reopen_status] = open_small_nvme(home.c_str());
    if (!expect_true(!reopen_status.failed())) return {};
    stored_documents = references_of(reopened.handle(), drive_collection);
    compare_all("after a reopen");
    return {};
}

#pragma endregion Cases

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    start_test_run(argc, argv);
    auto [directory, directory_status] = scratch_directory_t::make(test_run.settings.directory);
    if (us::st::failed(directory_status)) {
        fmt::println(stderr, "  the working directory would not open");
        return 1;
    }
    config_t const config {test_run.settings.engine, directory.path()};

    run_test_case("test_json_fold_codec", test_json_fold_codec);
    run_test_case("test_json_fold_shapes", test_json_fold_shapes);
    run_test_case(engine_label_t {"test_json_fold_reference"}.c_str(), test_json_fold_reference, config);
    run_test_case("test_json_fold_compose", test_json_fold_compose);
    run_test_case(engine_label_t {"test_json_fold_own_changes"}.c_str(), test_json_fold_own_changes, config);
    run_test_case(engine_label_t {"test_json_fold_maintenance"}.c_str(), test_json_fold_maintenance, config);

    return finish_test_run();
}
