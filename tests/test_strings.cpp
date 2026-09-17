/**
 *  @file tests/test_strings.cpp
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief Unit tests of the strings modality, which maps variable-length strings onto arbitrary bytes.
 *
 *  Every case runs against the main collection, which every engine holds, so the same suite covers the
 *  engines that refuse collections of their own. Strings carry explicit lengths throughout, so a zero
 *  byte inside one is stored and read back like any other.
 */

#include <cstddef> // `std::size_t`
#include <cstdio>  // `stderr`
#include <cstring> // `std::memcpy`

#include <array>       // `std::array`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`

#include <fmt/format.h>

#include <ustore/ustore.h>

#include "test.hpp"

using namespace unum::ustore;
using namespace unum;

namespace {

/** Most tasks one case sends in a single batch. */
constexpr std::size_t batch_capacity_k = 64;

/** The strided arrays one batch of strings and values hands to the C ABI. */
struct strings_batch_t {

    /** Pointer to the bytes of every string. */
    std::array<ustore_str_view_t, batch_capacity_k> texts {};

    /** Length of every string. */
    std::array<ustore_length_t, batch_capacity_k> text_lengths {};

    /** Pointer to the bytes of every value, null where the task removes its string. */
    std::array<ustore_bytes_cptr_t, batch_capacity_k> values {};

    /** Length of every value. */
    std::array<ustore_length_t, batch_capacity_k> value_lengths {};

    /** Tasks the batch holds. */
    std::size_t count = 0;

    /** Adds a task writing @p value under @p text. */
    void assign(std::string_view text, std::string_view value) noexcept {
        texts[count] = text.data();
        text_lengths[count] = static_cast<ustore_length_t>(text.size());
        values[count] = reinterpret_cast<ustore_bytes_cptr_t>(value.data());
        value_lengths[count] = static_cast<ustore_length_t>(value.size());
        ++count;
    }

    /** Adds a task removing @p text, which a null value spells. */
    void erase(std::string_view text) noexcept {
        texts[count] = text.data();
        text_lengths[count] = static_cast<ustore_length_t>(text.size());
        values[count] = nullptr;
        value_lengths[count] = 0;
        ++count;
    }
};

/** Removes every key and value of the main collection. */
[[nodiscard]] std::size_t clear_database(ustore_database_t database) noexcept {
    ustore_error_t error {};
    ustore_collection_drop_t drop {};
    drop.db = database;
    drop.error = &error;
    drop.id = ustore_collection_main_k;
    drop.mode = ustore_drop_keys_vals_k;
    ustore_collection_drop(&drop);
    return expect(error.status == ustore_success_k);
}

/** Sends @p batch through @c ustore_strings_write and counts a failure when it is refused. */
[[nodiscard]] std::size_t write_batch(ustore_database_t database, ustore_arena_t* arena,
                                      strings_batch_t const& batch) noexcept {
    ustore_error_t error {};
    ustore_strings_write_t write {};
    write.db = database;
    write.error = &error;
    write.arena = arena;
    write.tasks_count = batch.count;
    write.strings = batch.texts.data();
    write.strings_stride = sizeof(ustore_str_view_t);
    write.strings_lengths = batch.text_lengths.data();
    write.strings_lengths_stride = sizeof(ustore_length_t);
    write.values = batch.values.data();
    write.values_stride = sizeof(ustore_bytes_cptr_t);
    write.values_lengths = batch.value_lengths.data();
    write.values_lengths_stride = sizeof(ustore_length_t);

    ustore_strings_write(&write);
    if (error.status) fmt::println(stderr, "  write failed: {}", error.message);
    return expect(error.status == ustore_success_k);
}

/** Writes one string and its value. */
[[nodiscard]] std::size_t write_one(ustore_database_t database, ustore_arena_t* arena, std::string_view text,
                                    std::string_view value) noexcept {
    strings_batch_t batch;
    batch.assign(text, value);
    return write_batch(database, arena, batch);
}

/** Removes one string. */
[[nodiscard]] std::size_t erase_one(ustore_database_t database, ustore_arena_t* arena, std::string_view text) noexcept {
    strings_batch_t batch;
    batch.erase(text);
    return write_batch(database, arena, batch);
}

/** The values one read exported, addressed by task. */
struct read_results_t {

    /** Length of every value, @c ustore_length_missing_k for a string the database does not hold. */
    ustore_length_t* lengths = nullptr;

    /** Offsets of the values inside @c values. */
    ustore_length_t* offsets = nullptr;

    /** The tape of values. */
    ustore_byte_t* values = nullptr;

    /** Whether the string of task @p index is present. */
    [[nodiscard]] bool present(std::size_t index) const noexcept { return lengths[index] != ustore_length_missing_k; }

    /** The value of task @p index as text. */
    [[nodiscard]] std::string_view value_of(std::size_t index) const noexcept {
        return {reinterpret_cast<char const*>(values) + offsets[index], lengths[index]};
    }
};

/** Reads the values of @p batch, counting a failure when the read is refused. */
[[nodiscard]] std::size_t read_batch(ustore_database_t database, ustore_arena_t* arena, strings_batch_t const& batch,
                                     read_results_t& results) noexcept {
    ustore_error_t error {};
    ustore_strings_read_t read {};
    read.db = database;
    read.error = &error;
    read.arena = arena;
    read.tasks_count = batch.count;
    read.strings = batch.texts.data();
    read.strings_stride = sizeof(ustore_str_view_t);
    read.strings_lengths = batch.text_lengths.data();
    read.strings_lengths_stride = sizeof(ustore_length_t);
    read.offsets = &results.offsets;
    read.lengths = &results.lengths;
    read.values = &results.values;

    ustore_strings_read(&read);
    if (error.status) fmt::println(stderr, "  read failed: {}", error.message);
    return expect(error.status == ustore_success_k);
}

/** The strings one scan exported, in the order of their text. */
struct scan_results_t {

    /** Strings the scan found. */
    ustore_length_t count = 0;

    /** Offsets of the found strings inside @c strings. */
    ustore_length_t* offsets = nullptr;

    /** The tape of found strings. */
    ustore_char_t* strings = nullptr;

    /** The string found at @p index. */
    [[nodiscard]] std::string_view operator[](std::size_t index) const noexcept {
        return {strings + offsets[index], offsets[index + 1] - offsets[index]};
    }
};

/** Walks @p prefix, counting a failure when the scan is refused. */
[[nodiscard]] std::size_t scan_prefix(ustore_database_t database, ustore_arena_t* arena, std::string_view prefix,
                                      ustore_length_t limit, scan_results_t& results) noexcept {
    ustore_error_t error {};
    ustore_str_view_t const prefix_pointer = prefix.data();
    auto const prefix_length = static_cast<ustore_length_t>(prefix.size());
    ustore_length_t* counts = nullptr;
    ustore_strings_scan_t scan {};
    scan.db = database;
    scan.error = &error;
    scan.arena = arena;
    scan.tasks_count = 1;
    scan.prefixes = &prefix_pointer;
    scan.prefixes_lengths = &prefix_length;
    scan.count_limits = &limit;
    scan.counts = &counts;
    scan.strings_offsets = &results.offsets;
    scan.strings = &results.strings;

    ustore_strings_scan(&scan);
    if (error.status) {
        fmt::println(stderr, "  scan failed: {}", error.message);
        return expect(error.status == ustore_success_k);
    }
    results.count = counts[0];
    return 0;
}

/** Whether a scan of @p prefix returns exactly @p wanted, in that order. */
[[nodiscard]] std::size_t expect_scan(ustore_database_t database, ustore_arena_t* arena, std::string_view prefix,
                                      std::span<std::string_view const> wanted) noexcept {
    std::size_t failures = 0;
    scan_results_t results;
    failures += scan_prefix(database, arena, prefix, static_cast<ustore_length_t>(batch_capacity_k), results);
    failures += expect_eq(static_cast<std::size_t>(results.count), wanted.size());
    if (results.count != wanted.size()) return failures;
    for (std::size_t index = 0; index != wanted.size(); ++index) failures += expect_eq(results[index], wanted[index]);
    return failures;
}

/** An open database and the arena its calls export through. */
struct fixture_t {

    /** The open database. */
    ustore_database_t database = nullptr;

    /** Reusable memory for the results. */
    ustore_arena_t arena = nullptr;

    fixture_t(fixture_t const&) = delete;
    fixture_t& operator=(fixture_t const&) = delete;

    explicit fixture_t(char const* directory) noexcept {
        test_config_t const config {directory};
        ustore_error_t error {};
        ustore_database_init_t init {};
        init.config = config.c_str();
        init.db = &database;
        init.error = &error;
        ustore_database_init(&init);
        if (error.status) fmt::println(stderr, "  failed to open the database: {}", error.message);
    }

    ~fixture_t() noexcept {
        ustore_arena_free(arena);
        ustore_database_free(database);
    }
};

/** Short strings order exactly as their text, whatever byte values they carry. */
std::size_t strings_short_ordering(char const* directory) {
    std::size_t failures = 0;
    fixture_t fixture {directory};
    if (!fixture.database) return failures + 1;
    failures += clear_database(fixture.database);

    // Written out of order, including a byte above the signed range, which the key encoding must not reorder
    std::array<std::string_view, 6> const written {"b", "a", "ab", "aa", "\x80", ""};
    strings_batch_t batch;
    for (std::string_view const text : written) batch.assign(text, "value");
    failures += write_batch(fixture.database, &fixture.arena, batch);

    std::array<std::string_view, 6> const ordered {"", "a", "aa", "ab", "b", "\x80"};
    failures += expect_scan(fixture.database, &fixture.arena, "", ordered);
    return failures;
}

/** A prefix scan returns the members of the prefix, in order, and nothing else. */
std::size_t strings_prefix_scan(char const* directory) {
    std::size_t failures = 0;
    fixture_t fixture {directory};
    if (!fixture.database) return failures + 1;
    failures += clear_database(fixture.database);

    strings_batch_t batch;
    batch.assign("user:2", "second");
    batch.assign("team:1", "team");
    batch.assign("user:10", "tenth");
    batch.assign("user:1", "first");
    failures += write_batch(fixture.database, &fixture.arena, batch);

    std::array<std::string_view, 3> const users {"user:1", "user:10", "user:2"};
    failures += expect_scan(fixture.database, &fixture.arena, "user:", users);

    std::array<std::string_view, 1> const teams {"team:1"};
    failures += expect_scan(fixture.database, &fixture.arena, "team:", teams);

    std::array<std::string_view, 0> const absent {};
    failures += expect_scan(fixture.database, &fixture.arena, "missing", absent);
    return failures;
}

/** Strings sharing a long leading run share one bucket, and each keeps its own value. */
std::size_t strings_shared_prefix(char const* directory) {
    std::size_t failures = 0;
    fixture_t fixture {directory};
    if (!fixture.database) return failures + 1;
    failures += clear_database(fixture.database);

    // Fifteen leading bytes in common, so the strings share a key at every key width the engine compiles with
    constexpr std::string_view shared_k = "shared-prefix!!";
    std::array<std::string_view, 3> const written {"shared-prefix!!alpha", "shared-prefix!!gamma",
                                                   "shared-prefix!!beta"};
    strings_batch_t batch;
    batch.assign(written[0], "first");
    batch.assign(written[1], "third");
    batch.assign(written[2], "second");
    failures += write_batch(fixture.database, &fixture.arena, batch);

    read_results_t results;
    failures += read_batch(fixture.database, &fixture.arena, batch, results);
    failures += expect(results.present(0) && results.value_of(0) == std::string_view("first"));
    failures += expect(results.present(1) && results.value_of(1) == std::string_view("third"));
    failures += expect(results.present(2) && results.value_of(2) == std::string_view("second"));

    std::array<std::string_view, 3> const ordered {"shared-prefix!!alpha", "shared-prefix!!beta",
                                                   "shared-prefix!!gamma"};
    failures += expect_scan(fixture.database, &fixture.arena, shared_k, ordered);
    return failures;
}

/** The empty string and a string holding a zero byte are ordinary keys. */
std::size_t strings_empty_and_embedded_zero(char const* directory) {
    std::size_t failures = 0;
    fixture_t fixture {directory};
    if (!fixture.database) return failures + 1;
    failures += clear_database(fixture.database);

    constexpr std::string_view with_zero_k {"a\0b", 3};
    constexpr std::string_view trailing_zero_k {"a\0", 2};
    constexpr std::string_view binary_value_k {"\0\1\0", 3};

    strings_batch_t batch;
    batch.assign("", "empty string");
    batch.assign(with_zero_k, binary_value_k);
    batch.assign(trailing_zero_k, "trailing");
    batch.assign("a", "plain");
    failures += write_batch(fixture.database, &fixture.arena, batch);

    read_results_t results;
    failures += read_batch(fixture.database, &fixture.arena, batch, results);
    failures += expect(results.present(0) && results.value_of(0) == std::string_view("empty string"));
    failures += expect(results.present(1) && results.value_of(1) == binary_value_k);
    failures += expect(results.present(2) && results.value_of(2) == std::string_view("trailing"));
    failures += expect(results.present(3) && results.value_of(3) == std::string_view("plain"));

    // The length inside the key tells the three strings apart, though their bytes share a run
    std::array<std::string_view, 3> const ordered {"a", trailing_zero_k, with_zero_k};
    failures += expect_scan(fixture.database, &fixture.arena, "a", ordered);
    return failures;
}

/** Writing a string again replaces its value, and a task without one removes it. */
std::size_t strings_update_and_remove(char const* directory) {
    std::size_t failures = 0;
    fixture_t fixture {directory};
    if (!fixture.database) return failures + 1;
    failures += clear_database(fixture.database);

    failures += write_one(fixture.database, &fixture.arena, "key", "first");
    strings_batch_t probe;
    probe.assign("key", "");

    read_results_t results;
    failures += read_batch(fixture.database, &fixture.arena, probe, results);
    failures += expect(results.present(0) && results.value_of(0) == std::string_view("first"));

    failures += write_one(fixture.database, &fixture.arena, "key", "second");
    failures += read_batch(fixture.database, &fixture.arena, probe, results);
    failures += expect(results.present(0) && results.value_of(0) == std::string_view("second"));

    // An empty value is stored and read back as present, unlike a removed one
    failures += write_one(fixture.database, &fixture.arena, "key", "");
    failures += read_batch(fixture.database, &fixture.arena, probe, results);
    failures += expect(results.present(0));
    failures += expect_eq(results.value_of(0).size(), 0u);

    failures += erase_one(fixture.database, &fixture.arena, "key");
    failures += read_batch(fixture.database, &fixture.arena, probe, results);
    failures += expect(!results.present(0));

    std::array<std::string_view, 0> const empty {};
    failures += expect_scan(fixture.database, &fixture.arena, "", empty);
    return failures;
}

/** A bucket grows past one entry and shrinks back, and disappears once its last string goes. */
std::size_t strings_bucket_growth(char const* directory) {
    std::size_t failures = 0;
    fixture_t fixture {directory};
    if (!fixture.database) return failures + 1;
    failures += clear_database(fixture.database);

    constexpr std::string_view shared_k = "shared-prefix!!";
    constexpr std::string_view first_k = "shared-prefix!!one";
    constexpr std::string_view second_k = "shared-prefix!!two";
    constexpr std::string_view third_k = "shared-prefix!!three";

    failures += write_one(fixture.database, &fixture.arena, first_k, "1");
    std::array<std::string_view, 1> const alone {first_k};
    failures += expect_scan(fixture.database, &fixture.arena, shared_k, alone);

    failures += write_one(fixture.database, &fixture.arena, second_k, "2");
    failures += write_one(fixture.database, &fixture.arena, third_k, "3");
    std::array<std::string_view, 3> const grown {first_k, third_k, second_k};
    failures += expect_scan(fixture.database, &fixture.arena, shared_k, grown);

    failures += erase_one(fixture.database, &fixture.arena, third_k);
    failures += erase_one(fixture.database, &fixture.arena, second_k);
    failures += expect_scan(fixture.database, &fixture.arena, shared_k, alone);

    // The surviving entry still reads back, so shrinking rewrote the bucket rather than truncating it
    strings_batch_t probe;
    probe.assign(first_k, "");
    read_results_t results;
    failures += read_batch(fixture.database, &fixture.arena, probe, results);
    failures += expect(results.present(0) && results.value_of(0) == std::string_view("1"));

    failures += erase_one(fixture.database, &fixture.arena, first_k);
    std::array<std::string_view, 0> const gone {};
    failures += expect_scan(fixture.database, &fixture.arena, shared_k, gone);
    failures += read_batch(fixture.database, &fixture.arena, probe, results);
    failures += expect(!results.present(0));
    return failures;
}

/** A batch touching one bucket many times keeps only the last write of every string. */
std::size_t strings_batched_bucket(char const* directory) {
    std::size_t failures = 0;
    fixture_t fixture {directory};
    if (!fixture.database) return failures + 1;
    failures += clear_database(fixture.database);

    constexpr std::string_view shared_k = "shared-prefix!!";
    strings_batch_t batch;
    batch.assign("shared-prefix!!a", "stale");
    batch.assign("shared-prefix!!b", "kept");
    batch.assign("shared-prefix!!a", "fresh");
    failures += write_batch(fixture.database, &fixture.arena, batch);

    strings_batch_t probe;
    probe.assign("shared-prefix!!a", "");
    probe.assign("shared-prefix!!b", "");
    read_results_t results;
    failures += read_batch(fixture.database, &fixture.arena, probe, results);
    failures += expect(results.present(0) && results.value_of(0) == std::string_view("fresh"));
    failures += expect(results.present(1) && results.value_of(1) == std::string_view("kept"));

    std::array<std::string_view, 2> const ordered {"shared-prefix!!a", "shared-prefix!!b"};
    failures += expect_scan(fixture.database, &fixture.arena, shared_k, ordered);
    return failures;
}

/** A scan stops at its limit and resumes from the string it is given. */
std::size_t strings_scan_pagination(char const* directory) {
    std::size_t failures = 0;
    fixture_t fixture {directory};
    if (!fixture.database) return failures + 1;
    failures += clear_database(fixture.database);

    strings_batch_t batch;
    batch.assign("page:a", "1");
    batch.assign("page:b", "2");
    batch.assign("page:c", "3");
    failures += write_batch(fixture.database, &fixture.arena, batch);

    scan_results_t first_page;
    failures += scan_prefix(fixture.database, &fixture.arena, "page:", 2, first_page);
    failures += expect_eq(static_cast<std::size_t>(first_page.count), 2u);
    if (first_page.count == 2) {
        failures += expect_eq(first_page[0], std::string_view("page:a"));
        failures += expect_eq(first_page[1], std::string_view("page:b"));
    }

    ustore_error_t error {};
    ustore_str_view_t const prefix_pointer = "page:";
    ustore_length_t const prefix_length = 5;
    ustore_str_view_t const resume_pointer = "page:c";
    ustore_length_t const resume_length = 6;
    ustore_length_t limit = 8;
    ustore_length_t* counts = nullptr;
    scan_results_t second_page;
    ustore_strings_scan_t scan {};
    scan.db = fixture.database;
    scan.error = &error;
    scan.arena = &fixture.arena;
    scan.tasks_count = 1;
    scan.prefixes = &prefix_pointer;
    scan.prefixes_lengths = &prefix_length;
    scan.start_strings = &resume_pointer;
    scan.start_strings_lengths = &resume_length;
    scan.count_limits = &limit;
    scan.counts = &counts;
    scan.strings_offsets = &second_page.offsets;
    scan.strings = &second_page.strings;

    ustore_strings_scan(&scan);
    failures += expect(error.status == ustore_success_k);
    if (!error.status) {
        failures += expect_eq(static_cast<std::size_t>(counts[0]), 1u);
        if (counts[0] == 1) failures += expect_eq(second_page[0], std::string_view("page:c"));
    }
    return failures;
}

} // namespace

int main(int argc, char** argv) {
    std::string_view parent;
    std::array<flag_t, 1> const flags {{directory_flag(parent)}};
    if (argument_status_t const status = parse_test_arguments(argc, argv, flags); status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;

    auto const maybe_test_directory = test_directory_t::make(parent);
    if (!maybe_test_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", parent);
        return 1;
    }
    char const* const directory = maybe_test_directory->path();
    if (directory) fmt::println("Will work in directory: {}", directory);
    else fmt::println("Will work with default configuration");

    std::size_t total = 0;
    total += report("strings.short_ordering", strings_short_ordering(directory));
    total += report("strings.prefix_scan", strings_prefix_scan(directory));
    total += report("strings.shared_prefix", strings_shared_prefix(directory));
    total += report("strings.empty_and_embedded_zero", strings_empty_and_embedded_zero(directory));
    total += report("strings.update_and_remove", strings_update_and_remove(directory));
    total += report("strings.bucket_growth", strings_bucket_growth(directory));
    total += report("strings.batched_bucket", strings_batched_bucket(directory));
    total += report("strings.scan_pagination", strings_scan_pagination(directory));

    return report("total", total) ? 1 : 0;
}
