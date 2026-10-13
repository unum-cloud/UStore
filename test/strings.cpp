/**
 *  @file test/strings.cpp
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief Unit tests of the strings modality, which maps variable-length strings onto arbitrary bytes.
 *
 *  Every case runs against a fresh collection of the strings modality. Strings carry explicit lengths
 *  throughout, so a zero byte inside one is stored and read back like any other.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdio>  // `stderr`

#include <array>       // `std::array`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`

#include <fmt/format.h>

#include "ustore/ranges.hpp" // `bits_view_t`, `strided_strings`, `tape_string`, `tape_slice`
#include "ustore/ustore.h"

#include "harness.hpp" // `suite_exit_code`, `expect`, `expect_eq`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum;

namespace unum::ustore::test {

/** Most tasks one case sends in a single batch. */
constexpr std::size_t batch_capacity_k = 64;

/** The strings and values of one batch, which the C ABI reads through accessors. */
struct strings_batch_t {

    /** Every string. */
    std::array<std::string_view, batch_capacity_k> texts {};

    /** Every value. */
    std::array<std::string_view, batch_capacity_k> values {};

    /** Tasks the batch holds. */
    std::size_t count = 0;

    /** Adds a task writing @p value under @p text. */
    void assign(std::string_view text, std::string_view value) noexcept {
        texts[count] = text;
        values[count] = value;
        ++count;
    }
};

/** An open database, a fresh strings collection in it, and the arena its calls export through. */
struct fixture_t {

    /** The open database. */
    ustore_database_t database = nullptr;

    /** The strings collection every call of a case addresses. */
    ustore_collection_id_t collection = ustore_collection_main_k;

    /** Reusable memory for the results. */
    ustore_arena_t arena = nullptr;

    fixture_t(fixture_t const&) = delete;
    fixture_t& operator=(fixture_t const&) = delete;

    explicit fixture_t(config_t const& config) noexcept {
        auto [opened, open_status] = open_raw(config);
        if (open_status.failed()) {
            fmt::println(stderr, "  failed to open the database: {}", open_status.message);
            return;
        }
        database = opened;
        ustore_outcome_t outcome {};
        ustore_collection_create_t create {};
        create.database = database;
        create.outcome = &outcome;
        create.modality = ustore_modality_strings_k;
        create.collection = &collection;
        ustore_collection_create(&create);
        if (outcome.status != ustore_status_success_k)
            fmt::println(stderr, "  failed to create a strings collection: {}", outcome.message);

        ustore_arena_init_t arena_init {.arena = &arena, .executor = nullptr, .outcome = &outcome};
        ustore_arena_init(&arena_init);
        if (outcome.status != ustore_status_success_k)
            fmt::println(stderr, "  failed to make an arena: {}", outcome.message);
    }

    ~fixture_t() noexcept {
        ustore_arena_free(arena);
        ustore_database_free(database);
    }

    /** Whether the database opened with its strings collection and the arena exists. */
    [[nodiscard]] bool ready() const noexcept { return database && collection != ustore_collection_main_k && arena; }
};

/** Sends @p batch through @c ustore_strings_write and counts a failure when it is refused. */
[[nodiscard]] std::size_t write_batch(fixture_t& fixture, strings_batch_t const& batch) noexcept {
    strided_strings<std::string_view> const texts {{batch.texts.data(), sizeof(std::string_view)}};
    strided_strings<std::string_view> const values {{batch.values.data(), sizeof(std::string_view)}};
    ustore_outcome_t outcome {};
    ustore_strings_write_t write {};
    write.database = fixture.database;
    write.outcome = &outcome;
    write.count = batch.count;
    write.collections = {&fixture.collection, 0};
    write.strings = texts.view();
    write.values = values.view();

    ustore_strings_write(&write);
    return expect_success(outcome);
}

/** Writes one string and its value. */
[[nodiscard]] std::size_t write_one(fixture_t& fixture, std::string_view text, std::string_view value) noexcept {
    strings_batch_t batch;
    batch.assign(text, value);
    return write_batch(fixture, batch);
}

/** Removes one string through @c ustore_strings_remove. */
[[nodiscard]] std::size_t erase_one(fixture_t& fixture, std::string_view text) noexcept {
    strided_strings<std::string_view> const texts {{&text, 0}};
    ustore_outcome_t outcome {};
    ustore_strings_remove_t remove {};
    remove.database = fixture.database;
    remove.outcome = &outcome;
    remove.count = 1;
    remove.collections = {&fixture.collection, 0};
    remove.strings = texts.view();

    ustore_strings_remove(&remove);
    return expect_success(outcome);
}

/** The values one read exported into the arena of the fixture, addressed by task. */
struct read_results_t {

    /** Bits marking the strings the database holds. */
    ustore_octet_t const* presences = nullptr;

    /** The tape of values. */
    ustore_strings_view_t values {};

    /** Whether the string of task @p index is present. */
    [[nodiscard]] bool present(std::size_t index) const noexcept { return bits_view_t {presences}[index]; }

    /** The value of task @p index as text. */
    [[nodiscard]] std::string_view value_of(std::size_t index) const noexcept {
        return string_of(tape_string(values, index));
    }
};

/** Reads the values of @p batch, counting a failure when the read is refused. */
[[nodiscard]] std::size_t read_batch(fixture_t& fixture, strings_batch_t const& batch,
                                     read_results_t& results) noexcept {
    strided_strings<std::string_view> const texts {{batch.texts.data(), sizeof(std::string_view)}};
    ustore_outcome_t outcome {};
    ustore_strings_read_t read {};
    read.database = fixture.database;
    read.outcome = &outcome;
    read.count = batch.count;
    read.collections = {&fixture.collection, 0};
    read.strings = texts.view();
    read.presences = &results.presences;
    read.values = &results.values;
    read.arena = fixture.arena;

    ustore_strings_read(&read);
    ustore_strings_view_t const& values = results.values;
    bool const exported = values.layout == ustore_strings_u64_tape_k ? values.data.u64_tape.offsets != nullptr
                                                                     : values.data.u32_tape.offsets != nullptr;
    return expect_success(outcome) + expect(!batch.count || (results.presences && exported));
}

/** The strings one scan exported into the arena of the fixture, in the order of their text. */
struct scan_results_t {

    /** Strings the scan found. */
    ustore_length_t count = 0;

    /** The tape of found strings. */
    ustore_strings_view_t strings {};

    /** The string found at @p index. */
    [[nodiscard]] std::string_view operator[](std::size_t index) const noexcept {
        return string_of(tape_string(strings, index));
    }
};

/** Walks @p prefix from @p start, at most @c batch_capacity_k strings, counting a failure when the scan is refused. */
[[nodiscard]] std::size_t scan_prefix(fixture_t& fixture, std::string_view prefix, ustore_length_t limit,
                                      scan_results_t& results, std::string_view start = {}) noexcept {
    strided_strings<std::string_view> const prefixes {{&prefix, 0}};
    strided_strings<std::string_view> const starts {{&start, 0}};
    ustore_length_t const* scan_offsets = nullptr;
    ustore_outcome_t outcome {};
    ustore_strings_scan_t scan {};
    scan.database = fixture.database;
    scan.outcome = &outcome;
    scan.count = 1;
    scan.collections = {&fixture.collection, 0};
    scan.prefixes = prefixes.view();
    if (!start.empty()) scan.start_strings = starts.view();
    scan.offsets = &scan_offsets;
    scan.strings = &results.strings;
    scan.arena = fixture.arena;
    scan.wanted = limit;

    ustore_strings_scan(&scan);
    if (std::size_t const failures = expect_success(outcome) + expect(scan_offsets != nullptr)) return failures;
    results.count = scan_offsets[1] - scan_offsets[0];
    results.strings = tape_slice(results.strings, scan_offsets[0]);
    return 0;
}

/** Whether a scan of @p prefix returns exactly @p wanted, in that order. */
[[nodiscard]] std::size_t expect_scan(fixture_t& fixture, std::string_view prefix,
                                      std::span<std::string_view const> wanted) noexcept {
    std::size_t failures = 0;
    scan_results_t results;
    failures += scan_prefix(fixture, prefix, static_cast<ustore_length_t>(batch_capacity_k), results);
    failures += expect_eq(static_cast<std::size_t>(results.count), wanted.size());
    if (results.count != wanted.size()) return failures;
    for (std::size_t index = 0; index != wanted.size(); ++index) failures += expect_eq(results[index], wanted[index]);
    return failures;
}

/** Short strings order exactly as their text, whatever byte values they carry. */
std::size_t strings_short_ordering(config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.ready()) return failures + 1;

    // Written out of order, including a byte above the signed range, which the key encoding must not reorder
    std::array<std::string_view, 6> const written {"b", "a", "ab", "aa", "\x80", ""};
    strings_batch_t batch;
    for (std::string_view const text : written) batch.assign(text, "value");
    failures += write_batch(fixture, batch);

    std::array<std::string_view, 6> const ordered {"", "a", "aa", "ab", "b", "\x80"};
    failures += expect_scan(fixture, "", ordered);
    return failures;
}

/** A prefix scan returns the members of the prefix, in order, and nothing else. */
std::size_t strings_prefix_scan(config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.ready()) return failures + 1;

    strings_batch_t batch;
    batch.assign("user:2", "second");
    batch.assign("team:1", "team");
    batch.assign("user:10", "tenth");
    batch.assign("user:1", "first");
    failures += write_batch(fixture, batch);

    std::array<std::string_view, 3> const users {"user:1", "user:10", "user:2"};
    failures += expect_scan(fixture, "user:", users);

    std::array<std::string_view, 1> const teams {"team:1"};
    failures += expect_scan(fixture, "team:", teams);

    std::array<std::string_view, 0> const absent {};
    failures += expect_scan(fixture, "missing", absent);
    return failures;
}

/** Strings sharing a long leading run share one bucket, and each keeps its own value. */
std::size_t strings_shared_prefix(config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.ready()) return failures + 1;

    // Fifteen leading bytes in common, so the strings share a key at every key width the engine compiles with
    constexpr std::string_view shared_k = "shared-prefix!!";
    std::array<std::string_view, 3> const written {"shared-prefix!!alpha", "shared-prefix!!gamma",
                                                   "shared-prefix!!beta"};
    strings_batch_t batch;
    batch.assign(written[0], "first");
    batch.assign(written[1], "third");
    batch.assign(written[2], "second");
    failures += write_batch(fixture, batch);

    read_results_t results;
    failures += read_batch(fixture, batch, results);
    failures += expect(results.present(0) && results.value_of(0) == std::string_view("first"));
    failures += expect(results.present(1) && results.value_of(1) == std::string_view("third"));
    failures += expect(results.present(2) && results.value_of(2) == std::string_view("second"));

    std::array<std::string_view, 3> const ordered {"shared-prefix!!alpha", "shared-prefix!!beta",
                                                   "shared-prefix!!gamma"};
    failures += expect_scan(fixture, shared_k, ordered);
    return failures;
}

/** The empty string and a string holding a zero byte are ordinary keys. */
std::size_t strings_empty_and_embedded_zero(config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.ready()) return failures + 1;

    constexpr std::string_view with_zero_k {"a\0b", 3};
    constexpr std::string_view trailing_zero_k {"a\0", 2};
    constexpr std::string_view binary_value_k {"\0\1\0", 3};

    strings_batch_t batch;
    batch.assign("", "empty string");
    batch.assign(with_zero_k, binary_value_k);
    batch.assign(trailing_zero_k, "trailing");
    batch.assign("a", "plain");
    failures += write_batch(fixture, batch);

    read_results_t results;
    failures += read_batch(fixture, batch, results);
    failures += expect(results.present(0) && results.value_of(0) == std::string_view("empty string"));
    failures += expect(results.present(1) && results.value_of(1) == binary_value_k);
    failures += expect(results.present(2) && results.value_of(2) == std::string_view("trailing"));
    failures += expect(results.present(3) && results.value_of(3) == std::string_view("plain"));

    // The length inside the key tells the three strings apart, though their bytes share a run
    std::array<std::string_view, 3> const ordered {"a", trailing_zero_k, with_zero_k};
    failures += expect_scan(fixture, "a", ordered);
    return failures;
}

/** Writing a string again replaces its value, and a removal takes it away. */
std::size_t strings_update_and_remove(config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.ready()) return failures + 1;

    failures += write_one(fixture, "key", "first");
    strings_batch_t probe;
    probe.assign("key", "");

    read_results_t results;
    failures += read_batch(fixture, probe, results);
    failures += expect(results.present(0) && results.value_of(0) == std::string_view("first"));

    failures += write_one(fixture, "key", "second");
    failures += read_batch(fixture, probe, results);
    failures += expect(results.present(0) && results.value_of(0) == std::string_view("second"));

    // An empty value is stored and read back as present, unlike a removed one
    failures += write_one(fixture, "key", "");
    failures += read_batch(fixture, probe, results);
    failures += expect(results.present(0));
    failures += expect_eq(results.value_of(0).size(), 0u);

    failures += erase_one(fixture, "key");
    failures += read_batch(fixture, probe, results);
    failures += expect(!results.present(0));

    std::array<std::string_view, 0> const empty {};
    failures += expect_scan(fixture, "", empty);
    return failures;
}

/** A bucket grows past one entry and shrinks back, and disappears once its last string goes. */
std::size_t strings_bucket_growth(config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.ready()) return failures + 1;

    constexpr std::string_view shared_k = "shared-prefix!!";
    constexpr std::string_view first_k = "shared-prefix!!one";
    constexpr std::string_view second_k = "shared-prefix!!two";
    constexpr std::string_view third_k = "shared-prefix!!three";

    failures += write_one(fixture, first_k, "1");
    std::array<std::string_view, 1> const alone {first_k};
    failures += expect_scan(fixture, shared_k, alone);

    failures += write_one(fixture, second_k, "2");
    failures += write_one(fixture, third_k, "3");
    std::array<std::string_view, 3> const grown {first_k, third_k, second_k};
    failures += expect_scan(fixture, shared_k, grown);

    failures += erase_one(fixture, third_k);
    failures += erase_one(fixture, second_k);
    failures += expect_scan(fixture, shared_k, alone);

    // The surviving entry still reads back, so shrinking rewrote the bucket rather than truncating it
    strings_batch_t probe;
    probe.assign(first_k, "");
    read_results_t results;
    failures += read_batch(fixture, probe, results);
    failures += expect(results.present(0) && results.value_of(0) == std::string_view("1"));

    failures += erase_one(fixture, first_k);
    std::array<std::string_view, 0> const gone {};
    failures += expect_scan(fixture, shared_k, gone);
    failures += read_batch(fixture, probe, results);
    failures += expect(!results.present(0));
    return failures;
}

/** A batch touching one bucket many times keeps only the last write of every string. */
std::size_t strings_batched_bucket(config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.ready()) return failures + 1;

    constexpr std::string_view shared_k = "shared-prefix!!";
    strings_batch_t batch;
    batch.assign("shared-prefix!!a", "stale");
    batch.assign("shared-prefix!!b", "kept");
    batch.assign("shared-prefix!!a", "fresh");
    failures += write_batch(fixture, batch);

    strings_batch_t probe;
    probe.assign("shared-prefix!!a", "");
    probe.assign("shared-prefix!!b", "");
    read_results_t results;
    failures += read_batch(fixture, probe, results);
    failures += expect(results.present(0) && results.value_of(0) == std::string_view("fresh"));
    failures += expect(results.present(1) && results.value_of(1) == std::string_view("kept"));

    std::array<std::string_view, 2> const ordered {"shared-prefix!!a", "shared-prefix!!b"};
    failures += expect_scan(fixture, shared_k, ordered);
    return failures;
}

/** A scan stops at its limit and resumes from the string it is given. */
std::size_t strings_scan_pagination(config_t const& config) {
    std::size_t failures = 0;
    fixture_t fixture {config};
    if (!fixture.ready()) return failures + 1;

    strings_batch_t batch;
    batch.assign("page:a", "1");
    batch.assign("page:b", "2");
    batch.assign("page:c", "3");
    failures += write_batch(fixture, batch);

    scan_results_t first_page;
    failures += scan_prefix(fixture, "page:", 2, first_page);
    failures += expect_eq(static_cast<std::size_t>(first_page.count), 2u);
    if (first_page.count == 2) {
        failures += expect_eq(first_page[0], std::string_view("page:a"));
        failures += expect_eq(first_page[1], std::string_view("page:b"));
    }

    scan_results_t second_page;
    std::size_t const scanned = scan_prefix(fixture, "page:", 8, second_page, "page:c");
    failures += scanned;
    if (scanned == 0) {
        failures += expect_eq(static_cast<std::size_t>(second_page.count), 1u);
        if (second_page.count == 1) failures += expect_eq(second_page[0], std::string_view("page:c"));
    }
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

    report(totals, "strings.short_ordering", strings_short_ordering, config);
    report(totals, "strings.prefix_scan", strings_prefix_scan, config);
    report(totals, "strings.shared_prefix", strings_shared_prefix, config);
    report(totals, "strings.empty_and_embedded_zero", strings_empty_and_embedded_zero, config);
    report(totals, "strings.update_and_remove", strings_update_and_remove, config);
    report(totals, "strings.bucket_growth", strings_bucket_growth, config);
    report(totals, "strings.batched_bucket", strings_batched_bucket, config);
    report(totals, "strings.scan_pagination", strings_scan_pagination, config);

    return suite_exit_code(totals);
}
