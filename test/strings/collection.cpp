/**
 *  @file test/strings/collection.cpp
 *  @author Ash Vardanian
 *  @date September 17, 2026
 *  @brief Unit tests of @c strings_collection_t, the C++ handle over the strings modality.
 *
 *  Every case runs against a fresh collection of the strings modality. Walks run with a
 *  read-ahead smaller than the collection too, so the paging a stream does is exercised beside
 *  its single-page config.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdio>  // `stderr`

#include <array>       // `std::array`
#include <map>         // `std::map`
#include <span>        // `std::span`
#include <string>      // `std::string`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`
#include <vector>      // `std::vector`

#include <fmt/format.h>

#include "ustore/ustore.hpp"

#include "harness.hpp" // `suite_exit_code`, `expect`, `expect_eq`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum;

namespace unum::ustore::test {

/** Strings a walk fetches per page when a case wants the paging exercised. */
constexpr std::size_t small_read_ahead_k = 2;

/** An open database and the strings handle its cases work through. */
struct fixture_t {

    /** The open database. */
    database_t database;

    /** A handle to the strings collection every case works in. */
    strings_collection_t strings;

    /** The name of that collection. */
    static constexpr std::string_view name_k = "strings";

    fixture_t(fixture_t const&) = delete;
    fixture_t& operator=(fixture_t const&) = delete;

    explicit fixture_t(config_t const& config) noexcept {
        auto [opened, open_status] = open_database<database_t>(config);
        if (open_status.failed()) {
            fmt::println(stderr, "  failed to open the database: {}", open_status.message);
            return;
        }
        database = std::move(opened);
        if (status_t const cleared = database.clear(ustore_drop_collection_k); cleared.failed()) {
            fmt::println(stderr, "  failed to clear the database: {}", cleared.message);
            return;
        }
        auto maybe_strings = database.create<strings_collection_t>(name_k);
        if (!maybe_strings) {
            fmt::println(stderr, "  failed to create a strings collection: {}", maybe_strings.status().message);
            return;
        }
        strings = std::move(*maybe_strings);
    }

    /** Whether the database opened with its strings collection. */
    [[nodiscard]] bool ready() const noexcept { return database && strings != ustore_collection_id_t {}; }
};

/** Counts one failure when the collection lacks @p text or maps it onto another key
 *  than @p wanted. */
[[nodiscard]] std::size_t expect_value(strings_collection_t& strings, std::string_view text,
                                       ustore_key_t wanted) noexcept {
    auto maybe_present = strings.present(text);
    if (!maybe_present) return expect(maybe_present.has_value());
    if (!*maybe_present) return expect(*maybe_present);
    auto maybe_value = strings.value(text);
    if (!maybe_value) return expect(maybe_value.has_value());
    return expect_eq(*maybe_value, wanted);
}

/** Counts one failure when the collection still holds @p text, or a read of it answers another key
 *  than the nil UUID. */
[[nodiscard]] std::size_t expect_missing(strings_collection_t& strings, std::string_view text) noexcept {
    auto maybe_present = strings.present(text);
    if (!maybe_present) return expect(maybe_present.has_value());
    if (*maybe_present) return expect(!*maybe_present);
    auto maybe_value = strings.value(text);
    if (!maybe_value) return expect(maybe_value.has_value());
    return expect_eq(*maybe_value, ustore_key_t {});
}

/** Counts one failure when a walk of @p prefix is not exactly @p wanted, in that order. */
[[nodiscard]] std::size_t expect_walk(strings_collection_t& strings, std::string_view prefix,
                                      std::span<std::string_view const> wanted,
                                      std::size_t read_ahead = strings_stream_t::default_read_ahead_k) noexcept {
    std::size_t failures = 0;
    auto maybe_stream = strings.prefix_begin(prefix, read_ahead);
    if (!maybe_stream) return expect(maybe_stream.has_value());

    std::size_t walked = 0;
    for (strings_stream_t& stream = *maybe_stream; !stream.is_end(); ++stream, ++walked)
        if (walked < wanted.size()) failures += expect_eq(stream.string(), wanted[walked]);
    failures += expect_eq(walked, wanted.size());

    auto maybe_size = strings.size(prefix);
    if (!maybe_size) return failures + expect(maybe_size.has_value());
    return failures + expect_eq(*maybe_size, wanted.size());
}

/** A walk of a prefix returns the members of the prefix, in order, and nothing else. */
std::size_t strings_collection_prefix_walk(config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.ready()) return failures + 1;

    std::array<value_view_t, 4> const texts {"user:2", "team:1", "user:10", "user:1"};
    std::array<ustore_key_t, 4> const values {key_from_integer(2), key_from_integer(100), key_from_integer(10),
                                              key_from_integer(1)};
    failures += expect(fixture.strings.assign(texts, values).succeeded());

    std::array<std::string_view, 3> const users {"user:1", "user:10", "user:2"};
    failures += expect_walk(fixture.strings, "user:", users);
    failures += expect_walk(fixture.strings, "user:", users, small_read_ahead_k);

    std::array<std::string_view, 1> const teams {"team:1"};
    failures += expect_walk(fixture.strings, "team:", teams);

    std::array<std::string_view, 0> const absent {};
    failures += expect_walk(fixture.strings, "missing", absent);

    // A batch read exports the key of every string into the arena it is given
    auto [arena, arena_status] = arena_t::make();
    if (arena_status.failed()) return failures + expect(arena_status.succeeded());
    auto maybe_values = fixture.strings.read(texts, arena);
    if (!maybe_values) return failures + expect(maybe_values.has_value());
    keys_view_t const read = *maybe_values;
    failures += expect_eq(read.size(), texts.size());
    for (std::size_t index = 0; index != texts.size(); ++index) failures += expect_eq(read[index], values[index]);

    // A batch removes the strings a batch wrote
    failures += expect(fixture.strings.remove(texts).succeeded());
    failures += expect_walk(fixture.strings, "", absent);
    failures += expect_missing(fixture.strings, "user:1");
    return failures;
}

/** A page stops at its limit and repeats the string the next page resumes from. */
std::size_t strings_collection_paged_walk(config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.ready()) return failures + 1;

    std::array<value_view_t, 3> const texts {"page:a", "page:b", "page:c"};
    std::array<ustore_key_t, 3> const values {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
    failures += expect(fixture.strings.assign(texts, values).succeeded());

    auto [first_arena, first_arena_status] = arena_t::make();
    auto [second_arena, second_arena_status] = arena_t::make();
    if (first_arena_status.failed() || second_arena_status.failed())
        return failures + expect(first_arena_status.succeeded() && second_arena_status.succeeded());
    auto maybe_first = fixture.strings.prefixed_page("page:", 2, first_arena);
    if (!maybe_first) return failures + expect(maybe_first.has_value());
    sz::strings_view_t const first_page = *maybe_first;
    failures += expect_eq(first_page.size(), 2u);
    if (first_page.size() != 2) return failures;
    failures += expect_eq(first_page[0], sz::string_view_t {"page:a"});
    failures += expect_eq(first_page[1], sz::string_view_t {"page:b"});

    // The page the walk resumes from is inclusive, so the string it starts at comes back once
    // more, and since that string lives in the arena of the first page, the second page lands
    // in another arena
    auto maybe_second = fixture.strings.prefixed_page("page:", 8, second_arena, std::string_view {first_page[1]});
    if (!maybe_second) return failures + expect(maybe_second.has_value());
    sz::strings_view_t const second_page = *maybe_second;
    failures += expect_eq(second_page.size(), 2u);
    if (second_page.size() != 2) return failures;
    failures += expect_eq(second_page[0], sz::string_view_t {"page:b"});
    failures += expect_eq(second_page[1], sz::string_view_t {"page:c"});

    // A stream pulls from a cursor, which resumes past the last string it handed out,
    // whatever its read-ahead
    std::array<std::string_view, 3> const ordered {"page:a", "page:b", "page:c"};
    failures += expect_walk(fixture.strings, "page:", ordered, small_read_ahead_k);
    failures += expect_walk(fixture.strings, "page:", ordered, 3);
    return failures;
}

/** What a case expects a strings collection to hold: every string it wrote and did not remove, with
 *  its last key. */
using strings_model_t = std::map<std::string, ustore_key_t>;

/** Counts the failures of reading every string of @p texts out of @p strings as @p model holds it,
 *  and of walking the strings @p model holds under @p prefix. */
[[nodiscard]] std::size_t expect_model(strings_collection_t& strings, std::span<std::string const> texts,
                                       strings_model_t const& model, std::string_view prefix) noexcept {
    std::size_t failures = 0;
    for (std::string const& text : texts) {
        strings_model_t::const_iterator const held = model.find(text);
        failures += held == model.end() ? expect_missing(strings, text) : expect_value(strings, text, held->second);
        if (failures != 0) return failures;
    }
    std::vector<std::string_view> prefixed;
    for (auto const& [text, value] : model)
        if (std::string_view {text}.starts_with(prefix)) prefixed.push_back(text);
    return expect_walk(strings, prefix, prefixed);
}

/** Strings sharing a head, short ones and one of 64 KiB read the same while their commits merge in
 *  memory, once flushes and compactions move them to disk with rewrites and removals in between,
 *  and once the database reopens. */
std::size_t strings_collection_survives_maintenance(config_t const& config) {
    std::size_t failures = 0;
    std::vector<std::string> texts;
    for (std::size_t index = 0; index != 150; ++index)
        texts.push_back(fmt::format("shared-prefix!!{:03}{}", index, std::string(index % 40, 'x')));
    for (std::size_t index = 0; index != 50; ++index) texts.push_back(fmt::format("s{}", index));
    std::string long_text(64 * 1024, '\0');
    for (std::size_t index = 0; index != long_text.size(); ++index)
        long_text[index] = static_cast<char>('a' + index % 26);
    texts.push_back(std::move(long_text));
    strings_model_t model;
    constexpr std::string_view shared_k = "shared-prefix!!";

    {
        fixture_t fixture {config};
        if (!fixture.ready()) return failures + 1;
        // Every string a commit of its own, so the memory runs merge generation after generation
        for (std::size_t index = 0; index != texts.size(); ++index) {
            ustore_key_t const value = key_from_integer(index + 1);
            failures += expect(fixture.strings.assign(std::string_view {texts[index]}, value).succeeded());
            model[texts[index]] = value;
        }
        failures += expect_model(fixture.strings, texts, model, shared_k);

        // Every round rewrites some strings, removes others, and flushes and compacts, so versions
        // of one string and its removal land on different levels before a compaction merges them
        for (std::size_t round = 0; round != 9 && failures == 0; ++round) {
            for (std::size_t index = round; index < texts.size(); index += 3) {
                ustore_key_t const value = key_from_integer((round + 1) * texts.size() + index + 1);
                failures += expect(fixture.strings.assign(std::string_view {texts[index]}, value).succeeded());
                model[texts[index]] = value;
            }
            for (std::size_t index = round; index < texts.size(); index += 7) {
                failures += expect(fixture.strings.remove(std::string_view {texts[index]}).succeeded());
                model.erase(texts[index]);
            }
            failures += expect(fixture.database.compact().succeeded());
            failures += expect_model(fixture.strings, texts, model, shared_k);
            failures += expect_model(fixture.strings, texts, model, "");
        }
    }
    if (failures != 0 || !config.directory()) return failures;

    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto maybe_strings = database.find<strings_collection_t>(fixture_t::name_k);
    if (!maybe_strings) return failures + expect(maybe_strings.has_value());
    failures += expect_model(*maybe_strings, texts, model, shared_k);
    constexpr ustore_key_t reopened_k = key_from_integer(1'000'000);
    failures += expect(maybe_strings->assign(std::string_view {texts[1]}, reopened_k).succeeded());
    model[texts[1]] = reopened_k;
    failures += expect_model(*maybe_strings, texts, model, "");

    // A clear walks the strings window by window, removing every one of them
    failures += expect(maybe_strings->clear().succeeded());
    return failures + expect_model(*maybe_strings, texts, strings_model_t {}, "");
}

/** A handle inside a transaction writes through it, and only a commit publishes the strings. */
std::size_t strings_collection_in_transaction(config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.ready()) return failures + 1;
    if (!fixture.database.supports_transactions()) return failures;

    auto maybe_transaction = fixture.database.transaction();
    if (!maybe_transaction) return failures + expect(maybe_transaction.has_value());

    auto maybe_inside = maybe_transaction->find<strings_collection_t>(fixture_t::name_k);
    if (!maybe_inside) return failures + expect(maybe_inside.has_value());
    constexpr ustore_key_t staged_k = key_from_integer(1);
    failures += expect(maybe_inside->assign("staged", staged_k).succeeded());
    failures += expect_missing(fixture.strings, "staged");
    failures += expect(maybe_transaction->commit().has_value());
    failures += expect_value(fixture.strings, "staged", staged_k);
    return failures;
}

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    auto const maybe_test_directory = directory_t::make(options.directory);
    if (!maybe_test_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", options.directory);
        return 1;
    }
    char const* const directory = maybe_test_directory->path();
    if (directory) fmt::println("Will work in directory: {}", directory);
    else fmt::println("Will work with default configuration");
    config_t const config {options.engine, directory};

    report(totals, "strings_collection.prefix_walk", strings_collection_prefix_walk, config);
    report(totals, "strings_collection.paged_walk", strings_collection_paged_walk, config);
    report(totals, "strings_collection.in_transaction", strings_collection_in_transaction, config);
    report(totals, "strings_collection.survives_maintenance", strings_collection_survives_maintenance, config);

    return suite_exit_code(totals);
}
