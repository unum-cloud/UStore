/**
 *  @file test/strings_collection.cpp
 *  @author Ash Vardanian
 *  @date September 17, 2026
 *  @brief Unit tests of @c strings_collection_t, the C++ handle over the strings modality.
 *
 *  Every case runs against the main collection, which every engine holds, so the same suite covers the
 *  engines that refuse collections of their own. Walks run with a read-ahead smaller than the collection
 *  too, so the paging a stream does is exercised beside its single-page config.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdio>  // `stderr`

#include <array>       // `std::array`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`

#include <fmt/format.h>

#include <ustore/ustore.hpp>

#include "test.hpp" // `suite_exit_code`, `expect`, `expect_eq`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum;

namespace {

/** Strings a walk fetches per page when a case wants the paging exercised. */
constexpr std::size_t small_read_ahead_k = 2;

/** The value every case writes when the value itself does not matter. */
constexpr std::string_view filler_k = "value";

/** An open database and the strings handle its cases work through. */
struct fixture_t {

    /** The open database. */
    database_t database;

    /** A handle to the strings of the main collection. */
    strings_collection_t strings;

    fixture_t(fixture_t const&) = delete;
    fixture_t& operator=(fixture_t const&) = delete;

    explicit fixture_t(test_config_t const& config) noexcept {
        if (status_t const opened = open_database(database, config); opened.failed()) {
            fmt::println(stderr, "  failed to open the database: {}", opened.message);
            return;
        }
        strings = database.main<strings_collection_t>();
    }

    /** Whether the database opened and its strings start empty. */
    [[nodiscard]] bool ready() noexcept { return database && strings.clear().succeeded(); }
};

/** Counts one failure when the value of @p text is not @p wanted. */
[[nodiscard]] std::size_t expect_value(strings_collection_t& strings, std::string_view text,
                                       std::string_view wanted) noexcept {
    auto maybe_value = strings.value(text);
    if (!maybe_value) return expect(maybe_value.has_value());
    if (!*maybe_value) return expect(static_cast<bool>(*maybe_value));
    return expect_eq(std::string_view {*maybe_value}, wanted);
}

/** Counts one failure when the collection still holds @p text. */
[[nodiscard]] std::size_t expect_missing(strings_collection_t& strings, std::string_view text) noexcept {
    auto maybe_present = strings.present(text);
    if (!maybe_present) return expect(maybe_present.has_value());
    return expect(!*maybe_present);
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

/** Short strings order exactly as their text, whatever byte values they carry. */
std::size_t strings_collection_short_ordering(test_config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.ready()) return failures + 1;

    // Written out of order, including a byte above the signed range, which the key encoding must not reorder
    std::array<std::string_view, 6> const written {"b", "a", "ab", "aa", "\x80", ""};
    for (std::string_view const text : written) failures += expect(fixture.strings.assign(text, filler_k).succeeded());

    std::array<std::string_view, 6> const ordered {"", "a", "aa", "ab", "b", "\x80"};
    failures += expect_walk(fixture.strings, "", ordered);
    failures += expect_walk(fixture.strings, "", ordered, small_read_ahead_k);
    return failures;
}

/** A walk of a prefix returns the members of the prefix, in order, and nothing else. */
std::size_t strings_collection_prefix_walk(test_config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.ready()) return failures + 1;

    std::array<value_view_t, 4> const texts {"user:2", "team:1", "user:10", "user:1"};
    std::array<value_view_t, 4> const values {"second", "team", "tenth", "first"};
    failures += expect(fixture.strings.assign(texts, values).succeeded());

    std::array<std::string_view, 3> const users {"user:1", "user:10", "user:2"};
    failures += expect_walk(fixture.strings, "user:", users);
    failures += expect_walk(fixture.strings, "user:", users, small_read_ahead_k);

    std::array<std::string_view, 1> const teams {"team:1"};
    failures += expect_walk(fixture.strings, "team:", teams);

    std::array<std::string_view, 0> const absent {};
    failures += expect_walk(fixture.strings, "missing", absent);

    // A batch read addresses every value by its own offset and length
    auto maybe_values = fixture.strings.values(texts);
    if (!maybe_values) return failures + expect(maybe_values.has_value());
    embedded_blobs_t const read = *maybe_values;
    failures += expect_eq(read.size(), texts.size());
    for (std::size_t index = 0; index != texts.size(); ++index)
        failures += expect_eq(std::string_view {read[index]}, std::string_view {values[index]});
    return failures;
}

/** Strings sharing a long leading run share one bucket, and each keeps its own value. */
std::size_t strings_collection_shared_prefix(test_config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.ready()) return failures + 1;

    // Fifteen leading bytes in common, so the strings share a key at every key width the engine compiles with
    constexpr std::string_view shared_k = "shared-prefix!!";
    failures += expect(fixture.strings.assign("shared-prefix!!alpha", "first").succeeded());
    failures += expect(fixture.strings.assign("shared-prefix!!gamma", "third").succeeded());
    failures += expect(fixture.strings.assign("shared-prefix!!beta", "second").succeeded());

    failures += expect_value(fixture.strings, "shared-prefix!!alpha", "first");
    failures += expect_value(fixture.strings, "shared-prefix!!gamma", "third");
    failures += expect_value(fixture.strings, "shared-prefix!!beta", "second");

    std::array<std::string_view, 3> const ordered {"shared-prefix!!alpha", "shared-prefix!!beta",
                                                   "shared-prefix!!gamma"};
    failures += expect_walk(fixture.strings, shared_k, ordered);
    failures += expect_walk(fixture.strings, shared_k, ordered, small_read_ahead_k);
    return failures;
}

/** The empty string and a string holding a zero byte are ordinary keys. */
std::size_t strings_collection_empty_and_embedded_zero(test_config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.ready()) return failures + 1;

    constexpr std::string_view with_zero_k {"a\0b", 3};
    constexpr std::string_view trailing_zero_k {"a\0", 2};
    constexpr std::string_view binary_value_k {"\0\1\0", 3};

    failures += expect(fixture.strings.assign("", "empty string").succeeded());
    failures += expect(fixture.strings.assign(with_zero_k, binary_value_k).succeeded());
    failures += expect(fixture.strings.assign(trailing_zero_k, "trailing").succeeded());
    failures += expect(fixture.strings.assign("a", "plain").succeeded());

    failures += expect_value(fixture.strings, "", "empty string");
    failures += expect_value(fixture.strings, with_zero_k, binary_value_k);
    failures += expect_value(fixture.strings, trailing_zero_k, "trailing");
    failures += expect_value(fixture.strings, "a", "plain");

    // The length inside the key tells the three strings apart, though their bytes share a run
    std::array<std::string_view, 3> const ordered {"a", trailing_zero_k, with_zero_k};
    failures += expect_walk(fixture.strings, "a", ordered);
    failures += expect_walk(fixture.strings, "a", ordered, small_read_ahead_k);
    return failures;
}

/** Writing a string again replaces its value, and erasing it removes the string. */
std::size_t strings_collection_update_and_remove(test_config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.ready()) return failures + 1;

    failures += expect(fixture.strings.assign("key", "first").succeeded());
    failures += expect_value(fixture.strings, "key", "first");

    failures += expect(fixture.strings.assign("key", "second").succeeded());
    failures += expect_value(fixture.strings, "key", "second");

    // An empty value is stored and read back as present, unlike a removed one
    failures += expect(fixture.strings.assign("key", "").succeeded());
    failures += expect_value(fixture.strings, "key", "");

    failures += expect(fixture.strings.erase("key").succeeded());
    failures += expect_missing(fixture.strings, "key");

    std::array<std::string_view, 0> const empty {};
    failures += expect_walk(fixture.strings, "", empty);

    // A batch erases the strings a batch wrote
    std::array<value_view_t, 2> const texts {"first", "second"};
    std::array<value_view_t, 2> const values {"1", "2"};
    failures += expect(fixture.strings.assign(texts, values).succeeded());
    failures += expect(fixture.strings.erase(texts).succeeded());
    failures += expect_missing(fixture.strings, "first");
    failures += expect_missing(fixture.strings, "second");
    return failures;
}

/** A bucket grows past one entry and shrinks back, and disappears once its last string goes. */
std::size_t strings_collection_bucket_growth(test_config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.ready()) return failures + 1;

    constexpr std::string_view shared_k = "shared-prefix!!";
    constexpr std::string_view first_k = "shared-prefix!!one";
    constexpr std::string_view second_k = "shared-prefix!!two";
    constexpr std::string_view third_k = "shared-prefix!!three";

    failures += expect(fixture.strings.assign(first_k, "1").succeeded());
    std::array<std::string_view, 1> const alone {first_k};
    failures += expect_walk(fixture.strings, shared_k, alone);

    failures += expect(fixture.strings.assign(second_k, "2").succeeded());
    failures += expect(fixture.strings.assign(third_k, "3").succeeded());
    std::array<std::string_view, 3> const grown {first_k, third_k, second_k};
    failures += expect_walk(fixture.strings, shared_k, grown);
    failures += expect_walk(fixture.strings, shared_k, grown, small_read_ahead_k);

    failures += expect(fixture.strings.erase(third_k).succeeded());
    failures += expect(fixture.strings.erase(second_k).succeeded());
    failures += expect_walk(fixture.strings, shared_k, alone);

    // The surviving entry still reads back, so shrinking rewrote the bucket rather than truncating it
    failures += expect_value(fixture.strings, first_k, "1");

    failures += expect(fixture.strings.erase(first_k).succeeded());
    std::array<std::string_view, 0> const gone {};
    failures += expect_walk(fixture.strings, shared_k, gone);
    failures += expect_missing(fixture.strings, first_k);
    return failures;
}

/** A page stops at its limit and repeats the string the next page resumes from. */
std::size_t strings_collection_paged_walk(test_config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.ready()) return failures + 1;

    std::array<value_view_t, 3> const texts {"page:a", "page:b", "page:c"};
    std::array<value_view_t, 3> const values {"1", "2", "3"};
    failures += expect(fixture.strings.assign(texts, values).succeeded());

    auto maybe_first = fixture.strings.prefixed_page("page:", 2);
    if (!maybe_first) return failures + expect(maybe_first.has_value());
    joined_strs_t const first_page = *maybe_first;
    failures += expect_eq(first_page.size(), 2u);
    if (first_page.size() != 2) return failures;
    failures += expect_eq(first_page[0], std::string_view {"page:a"});
    failures += expect_eq(first_page[1], std::string_view {"page:b"});

    // The page the walk resumes from is inclusive, so the string it starts at comes back once more
    auto maybe_second = fixture.strings.prefixed_page("page:", 8, first_page[1]);
    if (!maybe_second) return failures + expect(maybe_second.has_value());
    joined_strs_t const second_page = *maybe_second;
    failures += expect_eq(second_page.size(), 2u);
    if (second_page.size() != 2) return failures;
    failures += expect_eq(second_page[0], std::string_view {"page:b"});
    failures += expect_eq(second_page[1], std::string_view {"page:c"});

    // A stream hides that repetition, whatever its read-ahead
    std::array<std::string_view, 3> const ordered {"page:a", "page:b", "page:c"};
    failures += expect_walk(fixture.strings, "page:", ordered, small_read_ahead_k);
    failures += expect_walk(fixture.strings, "page:", ordered, 3);
    return failures;
}

/** A handle inside a transaction writes through it, and only a commit publishes the strings. */
std::size_t strings_collection_in_transaction(test_config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.ready()) return failures + 1;
    if (!fixture.database.supports_transactions()) return failures;

    auto maybe_transaction = fixture.database.transact();
    if (!maybe_transaction) return failures + expect(maybe_transaction.has_value());

    strings_collection_t inside = maybe_transaction->main<strings_collection_t>();
    failures += expect(inside.assign("pending", "value").succeeded());
    failures += expect_missing(fixture.strings, "pending");
    failures += expect(maybe_transaction->commit().succeeded());
    failures += expect_value(fixture.strings, "pending", "value");
    return failures;
}

} // namespace

int main(int argc, char** argv) {
    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    auto const maybe_test_directory = test_directory_t::make(options.directory);
    if (!maybe_test_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", options.directory);
        return 1;
    }
    char const* const directory = maybe_test_directory->path();
    if (directory) fmt::println("Will work in directory: {}", directory);
    else fmt::println("Will work with default configuration");
    test_config_t const config {options.engine, directory, options.engine_config};

    report(totals, "strings_collection.short_ordering", strings_collection_short_ordering, config);
    report(totals, "strings_collection.prefix_walk", strings_collection_prefix_walk, config);
    report(totals, "strings_collection.shared_prefix", strings_collection_shared_prefix, config);
    report(totals, "strings_collection.empty_and_embedded_zero", strings_collection_empty_and_embedded_zero, config);
    report(totals, "strings_collection.update_and_remove", strings_collection_update_and_remove, config);
    report(totals, "strings_collection.bucket_growth", strings_collection_bucket_growth, config);
    report(totals, "strings_collection.paged_walk", strings_collection_paged_walk, config);
    report(totals, "strings_collection.in_transaction", strings_collection_in_transaction, config);

    return suite_exit_code(totals);
}
