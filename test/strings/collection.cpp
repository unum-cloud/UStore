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

#include "harness.hpp" // `run_test_case`, `expect_true`, `expect_equal`, `expect_status`

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
        if (!expect_status(open_status, status_code_t::success_k)) return;
        database = std::move(opened);
        if (!expect_status(database.clear(ustore_drop_collection_k), status_code_t::success_k)) return;
        auto maybe_strings = database.create<strings_collection_t>(name_k);
        if (!expect_status(maybe_strings, status_code_t::success_k)) return;
        strings = std::move(maybe_strings.value);
    }

    /** Whether the database opened with its strings collection. */
    [[nodiscard]] bool ready() const noexcept {
        return database.handle() && !(strings.handle() == ustore_collection_id_t {});
    }
};

/** Holds when the collection maps @p text onto @p wanted, failing a check otherwise. */
bool expect_value(strings_collection_t& strings, std::string_view text, ustore_key_t wanted) noexcept {
    auto maybe_present = strings.present(text);
    if (!expect_true(!maybe_present.status.failed()) || !expect_true(maybe_present.value)) return false;
    auto maybe_value = strings.value(text);
    if (!expect_true(!maybe_value.status.failed())) return false;
    return expect_equal(maybe_value.value, wanted);
}

/** Holds when the collection lacks @p text and a read of it answers the nil UUID. */
bool expect_missing(strings_collection_t& strings, std::string_view text) noexcept {
    auto maybe_present = strings.present(text);
    if (!expect_true(!maybe_present.status.failed()) || !expect_true(!maybe_present.value)) return false;
    auto maybe_value = strings.value(text);
    if (!expect_true(!maybe_value.status.failed())) return false;
    return expect_equal(maybe_value.value, ustore_key_t {});
}

/** Holds when a walk of @p prefix is exactly @p wanted, in that order, and its size says so too. */
bool expect_walk(strings_collection_t& strings, std::string_view prefix, std::span<std::string_view const> wanted,
                 std::size_t read_ahead = strings_stream_t::default_read_ahead_k) noexcept {
    auto maybe_stream = strings.prefix_begin(prefix, read_ahead);
    if (!expect_true(!maybe_stream.status.failed())) return false;

    std::size_t walked = 0;
    bool held = true;
    for (strings_stream_t& stream = maybe_stream.value; !stream.is_end(); ++stream, ++walked)
        if (walked < wanted.size()) held = expect_equal(stream.string(), wanted[walked]) && held;
    held = expect_equal(walked, wanted.size()) && held;

    auto maybe_size = strings.size(prefix);
    if (!expect_true(!maybe_size.status.failed())) return false;
    return expect_equal(maybe_size.value, wanted.size()) && held;
}

/** A walk of a prefix returns the members of the prefix, in order, and nothing else. */
void test_strings_collection_prefix_walk(config_t const& config) {
    fixture_t fixture {config};
    if (!fixture.ready()) return;

    std::array<value_view_t, 4> const texts {"user:2", "team:1", "user:10", "user:1"};
    std::array<ustore_key_t, 4> const values {key_from_integer(2), key_from_integer(100), key_from_integer(10),
                                              key_from_integer(1)};
    expect_true(!fixture.strings.write(texts, values).failed());

    std::array<std::string_view, 3> const users {"user:1", "user:10", "user:2"};
    expect_walk(fixture.strings, "user:", users);
    expect_walk(fixture.strings, "user:", users, small_read_ahead_k);

    std::array<std::string_view, 1> const teams {"team:1"};
    expect_walk(fixture.strings, "team:", teams);

    std::array<std::string_view, 0> const absent {};
    expect_walk(fixture.strings, "missing", absent);

    // A batch read exports the key of every string into the arena it is given
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(!arena_status.failed())) return;
    auto maybe_values = fixture.strings.read(texts, arena);
    if (!expect_true(!maybe_values.status.failed())) return;
    keys_view_t const read = maybe_values.value;
    expect_equal(read.size(), texts.size());
    for (std::size_t index = 0; index != texts.size(); ++index) expect_equal(read[index], values[index]);

    // A batch removes the strings a batch wrote
    expect_true(!fixture.strings.remove(texts).failed());
    expect_walk(fixture.strings, "", absent);
    expect_missing(fixture.strings, "user:1");
}

/** A page stops at its limit and repeats the string the next page resumes from. */
void test_strings_collection_paged_walk(config_t const& config) {
    fixture_t fixture {config};
    if (!fixture.ready()) return;

    std::array<value_view_t, 3> const texts {"page:a", "page:b", "page:c"};
    std::array<ustore_key_t, 3> const values {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
    expect_true(!fixture.strings.write(texts, values).failed());

    auto [first_arena, first_arena_status] = arena_t::make();
    auto [second_arena, second_arena_status] = arena_t::make();
    if (!expect_true(!first_arena_status.failed() && !second_arena_status.failed())) return;
    auto maybe_first = fixture.strings.prefixed_page("page:", 2, first_arena);
    if (!expect_true(!maybe_first.status.failed())) return;
    strings_view_t const first_page = maybe_first.value;
    if (!expect_equal(first_page.size(), 2u)) return;
    expect_equal(first_page[0], std::string_view {"page:a"});
    expect_equal(first_page[1], std::string_view {"page:b"});

    // The page the walk resumes from is inclusive, so the string it starts at comes back once
    // more, and since that string lives in the arena of the first page, the second page lands
    // in another arena
    auto maybe_second = fixture.strings.prefixed_page("page:", 8, second_arena, std::string_view {first_page[1]});
    if (!expect_true(!maybe_second.status.failed())) return;
    strings_view_t const second_page = maybe_second.value;
    if (!expect_equal(second_page.size(), 2u)) return;
    expect_equal(second_page[0], std::string_view {"page:b"});
    expect_equal(second_page[1], std::string_view {"page:c"});

    // A stream pulls from a cursor, which resumes past the last string it handed out,
    // whatever its read-ahead
    std::array<std::string_view, 3> const ordered {"page:a", "page:b", "page:c"};
    expect_walk(fixture.strings, "page:", ordered, small_read_ahead_k);
    expect_walk(fixture.strings, "page:", ordered, 3);
}

/** What a case expects a strings collection to hold: every string it wrote and did not remove, with
 *  its last key. */
using strings_model_t = std::map<std::string, ustore_key_t>;

/** Holds when every string of @p texts reads out of @p strings as @p model holds it, stopping at
 *  the first that does not, and the strings @p model holds under @p prefix walk in order. */
bool expect_model(strings_collection_t& strings, std::span<std::string const> texts, strings_model_t const& model,
                  std::string_view prefix) noexcept {
    for (std::string const& text : texts) {
        strings_model_t::const_iterator const held = model.find(text);
        if (!(held == model.end() ? expect_missing(strings, text) : expect_value(strings, text, held->second)))
            return false;
    }
    std::vector<std::string_view> prefixed;
    for (auto const& [text, value] : model)
        if (std::string_view {text}.starts_with(prefix)) prefixed.push_back(text);
    return expect_walk(strings, prefix, prefixed);
}

/** Strings sharing a head, short ones and one of 64 KiB read the same while their commits merge in
 *  memory, once flushes and compactions move them to disk with rewrites and removals in between,
 *  and once the database reopens. */
void test_strings_collection_survives_maintenance(config_t const& config) {
    std::vector<std::string> texts;
    for (std::size_t index = 0; index != 150; ++index)
        texts.push_back(fmt::format("shared-prefix{:03}{}", index, std::string(index % 40, 'x')));
    for (std::size_t index = 0; index != 50; ++index) texts.push_back(fmt::format("s{}", index));
    std::string long_text(64 * 1024, '\0');
    for (std::size_t index = 0; index != long_text.size(); ++index)
        long_text[index] = static_cast<char>('a' + index % 26);
    texts.push_back(std::move(long_text));
    strings_model_t model;
    constexpr std::string_view shared_k = "shared-prefix";

    bool held = true;
    {
        fixture_t fixture {config};
        if (!fixture.ready()) return;
        // Every string a commit of its own, so the memory runs merge generation after generation
        for (std::size_t index = 0; index != texts.size(); ++index) {
            ustore_key_t const value = key_from_integer(index + 1);
            held = expect_true(!fixture.strings.assign(std::string_view {texts[index]}, value).failed()) && held;
            model[texts[index]] = value;
        }
        held = expect_model(fixture.strings, texts, model, shared_k) && held;

        // Every round rewrites some strings, removes others, and flushes and compacts, so versions
        // of one string and its removal land on different levels before a compaction merges them
        for (std::size_t round = 0; round != 9 && held; ++round) {
            for (std::size_t index = round; index < texts.size(); index += 3) {
                ustore_key_t const value = key_from_integer((round + 1) * texts.size() + index + 1);
                held = expect_true(!fixture.strings.assign(std::string_view {texts[index]}, value).failed()) && held;
                model[texts[index]] = value;
            }
            for (std::size_t index = round; index < texts.size(); index += 7) {
                held = expect_true(!fixture.strings.remove(std::string_view {texts[index]}).failed()) && held;
                model.erase(texts[index]);
            }
            held = expect_true(!fixture.database.compact().failed()) && held;
            held = expect_model(fixture.strings, texts, model, shared_k) && held;
            held = expect_model(fixture.strings, texts, model, "") && held;
        }
    }
    if (!held || !config.directory()) return;

    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(!open_status.failed())) return;
    auto maybe_strings = database.find<strings_collection_t>(fixture_t::name_k);
    if (!expect_true(!maybe_strings.status.failed())) return;
    expect_model(maybe_strings.value, texts, model, shared_k);
    constexpr ustore_key_t reopened_k = key_from_integer(1'000'000);
    expect_true(!maybe_strings.value.assign(std::string_view {texts[1]}, reopened_k).failed());
    model[texts[1]] = reopened_k;
    expect_model(maybe_strings.value, texts, model, "");

    // A clear walks the strings window by window, removing every one of them
    expect_true(!maybe_strings.value.clear().failed());
    expect_model(maybe_strings.value, texts, strings_model_t {}, "");
}

/** A handle inside a transaction writes through it, and only a commit publishes the strings. */
case_result_t test_strings_collection_in_transaction(config_t const& config) {
    fixture_t fixture {config};
    if (!fixture.ready()) return {};
    if (!probed(fixture.database.supports_transactions())) return case_result_t::skipped(needs_transactions_k);

    auto maybe_transaction = fixture.database.transaction();
    if (!expect_true(!maybe_transaction.status.failed())) return {};

    auto maybe_inside = maybe_transaction.value.find<strings_collection_t>(fixture_t::name_k);
    if (!expect_true(!maybe_inside.status.failed())) return {};
    constexpr ustore_key_t staged_k = key_from_integer(1);
    expect_true(!maybe_inside.value.assign("staged", staged_k).failed());
    expect_missing(fixture.strings, "staged");
    expect_true(!maybe_transaction.value.commit().status.failed());
    expect_value(fixture.strings, "staged", staged_k);
    return {};
}

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    start_test_run(argc, argv);

    auto [scratch, scratch_status] = scratch_directory_t::make(test_run.settings.directory);
    if (st::failed(scratch_status)) {
        fmt::println(stderr, "Cannot create a unique directory under {}", test_run.settings.directory);
        return 1;
    }
    char const* const directory = scratch.path();
    if (directory) fmt::println("Will work in directory: {}", directory);
    else fmt::println("Will work with default configuration");
    config_t const config {test_run.settings.engine, directory};

    run_test_case(engine_label_t {"test_strings_collection_prefix_walk"}.c_str(), test_strings_collection_prefix_walk,
                  config);
    run_test_case(engine_label_t {"test_strings_collection_paged_walk"}.c_str(), test_strings_collection_paged_walk,
                  config);
    run_test_case(engine_label_t {"test_strings_collection_in_transaction"}.c_str(),
                  test_strings_collection_in_transaction, config);
    run_test_case(engine_label_t {"test_strings_collection_survives_maintenance"}.c_str(),
                  test_strings_collection_survives_maintenance, config);

    return finish_test_run();
}
