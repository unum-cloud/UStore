/**
 *  @file test/abi/strings.cpp
 *  @author Ash Vardanian
 *  @date September 16, 2026
 *  @brief Unit tests of the strings modality, which maps variable-length strings onto keys of other collections.
 *
 *  Every case runs against a fresh collection of the strings modality. Strings hold explicit
 *  lengths throughout, so a zero byte inside one is stored and read back like any other, and every
 *  string maps onto a key the case derives from an integer.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdio>  // `stderr`

#include <algorithm>   // `std::find`
#include <array>       // `std::array`
#include <span>        // `std::span`
#include <string>      // `std::string`
#include <string_view> // `std::string_view`
#include <vector>      // `std::vector`

#include <fmt/format.h>

#include "ustore/ranges.hpp" // `bits_view_t`, `strided_strings`, `tape_string`, `tape_slice`
#include "ustore/ustore.h"

#include "abi/fixture.hpp" // `open_reference`
#include "harness.hpp"     // `run_test_case`, `expect_true`, `expect_equal`, `expect_success`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum;

namespace unum::ustore::test {

/** Most tasks one case sends in a single batch. */
constexpr std::size_t batch_capacity_k = 64;

/** The strings and keys of one batch, which the C ABI reads through an accessor and a strided view. */
struct strings_batch_t {

    /** Every string. */
    std::array<std::string_view, batch_capacity_k> texts {};

    /** The key every string maps onto. */
    std::array<ustore_key_t, batch_capacity_k> values {};

    /** Tasks the batch holds. */
    std::size_t count = 0;

    /** Adds a task mapping @p text onto @p value. */
    void assign(std::string_view text, ustore_key_t value) noexcept {
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
    ustore_collection_id_t collection {};

    /** Reusable memory for the results. */
    ustore_arena_t arena = nullptr;

    fixture_t(fixture_t const&) = delete;
    fixture_t& operator=(fixture_t const&) = delete;

    explicit fixture_t(config_t const& config) noexcept : fixture_t(open_raw(config)) {}

    /** Takes over the database @p opening answers and creates a strings collection in it. */
    explicit fixture_t(expected<ustore_database_t> const& opening) noexcept {
        auto [opened, open_status] = opening;
        if (!expect_status(open_status, status_code_t::success_k)) return;
        database = opened;
        ustore_outcome_t outcome {};
        ustore_collection_create_t create {};
        create.database = database;
        create.outcome = &outcome;
        create.modality = ustore_modality_strings_k;
        create.collection = &collection;
        ustore_collection_create(&create);
        expect_success(outcome);

        ustore_arena_init_t arena_init {.arena = &arena, .executor = nullptr, .outcome = &outcome};
        ustore_arena_init(&arena_init);
        expect_success(outcome);
    }

    ~fixture_t() noexcept {
        ustore_arena_free(arena);
        ustore_database_free(database);
    }

    /** Whether the database opened with its strings collection and the arena exists. */
    [[nodiscard]] bool ready() const noexcept { return database && collection != ustore_collection_id_t {} && arena; }
};

/** Maps @p texts onto @p values in one call of @c ustore_strings_write, through @p transaction unless it is null,
 *  and answers its outcome. */
[[nodiscard]] ustore_outcome_t write_strings(fixture_t& fixture, std::span<std::string_view const> texts,
                                             std::span<ustore_key_t const> values,
                                             ustore_transaction_t transaction = nullptr) noexcept {
    strided_strings<std::string_view> const strided_texts {{texts.data(), sizeof(std::string_view)}};
    ustore_outcome_t outcome {};
    ustore_strings_write_t write {};
    write.database = fixture.database;
    write.outcome = &outcome;
    write.transaction = transaction;
    write.count = texts.size();
    write.collections = {&fixture.collection, 0};
    write.strings = strided_texts.view();
    write.values = {values.data(), sizeof(ustore_key_t)};

    ustore_strings_write(&write);
    return outcome;
}

/** Sends @p batch through @c ustore_strings_write, failing a check when it is refused. */
bool write_batch(fixture_t& fixture, strings_batch_t const& batch) noexcept {
    ustore_outcome_t outcome = write_strings(fixture, std::span {batch.texts.data(), batch.count},
                                             std::span {batch.values.data(), batch.count});
    return expect_success(outcome);
}

/** Writes one string and the key it maps onto. */
bool write_one(fixture_t& fixture, std::string_view text, ustore_key_t value) noexcept {
    strings_batch_t batch;
    batch.assign(text, value);
    return write_batch(fixture, batch);
}

/** Removes @p texts in one call of @c ustore_strings_remove, failing a check when it is refused. */
bool erase_strings(fixture_t& fixture, std::span<std::string_view const> texts) noexcept {
    strided_strings<std::string_view> const strided_texts {{texts.data(), sizeof(std::string_view)}};
    ustore_outcome_t outcome {};
    ustore_strings_remove_t remove {};
    remove.database = fixture.database;
    remove.outcome = &outcome;
    remove.count = texts.size();
    remove.collections = {&fixture.collection, 0};
    remove.strings = strided_texts.view();

    ustore_strings_remove(&remove);
    return expect_success(outcome);
}

/** Removes one string through @c ustore_strings_remove. */
bool erase_one(fixture_t& fixture, std::string_view text) noexcept {
    return erase_strings(fixture, std::span {&text, 1});
}

/** A transaction the case frees whatever the case ends in. */
struct transaction_holder_t {

    /** The transaction, null until begun. */
    ustore_transaction_t transaction = nullptr;

    transaction_holder_t() noexcept = default;
    transaction_holder_t(transaction_holder_t const&) = delete;
    transaction_holder_t& operator=(transaction_holder_t const&) = delete;
    ~transaction_holder_t() noexcept {
        if (transaction) ustore_transaction_free(transaction);
    }
};

/** Begins a transaction of the fixture's database into @p holder, failing a check when
 *  it is refused. */
bool begin_transaction(fixture_t& fixture, transaction_holder_t& holder) noexcept {
    ustore_outcome_t outcome {};
    ustore_transaction_init_t init {};
    init.transaction = &holder.transaction;
    init.database = fixture.database;
    init.outcome = &outcome;
    ustore_transaction_init(&init);
    return expect_success(outcome);
}

/** Commits @p transaction of the fixture's database and answers the outcome. */
[[nodiscard]] ustore_outcome_t commit_transaction(fixture_t& fixture, ustore_transaction_t transaction) noexcept {
    ustore_outcome_t outcome {};
    ustore_transaction_commit_t commit {};
    commit.database = fixture.database;
    commit.outcome = &outcome;
    commit.transaction = transaction;
    ustore_transaction_commit(&commit);
    return outcome;
}

/** The keys one read exported into the arena of the fixture, addressed by task. */
struct read_results_t {

    /** Bits marking the strings the database holds. */
    ustore_octet_t const* presences = nullptr;

    /** The key every string maps onto, the nil UUID for a missing one. */
    ustore_key_t const* values = nullptr;

    /** Whether the string of task @p index is present. */
    [[nodiscard]] bool present(std::size_t index) const noexcept { return bits_view_t {presences}[index]; }

    /** The key the string of task @p index maps onto. */
    [[nodiscard]] ustore_key_t value_of(std::size_t index) const noexcept { return values[index]; }
};

/** Reads the keys @p texts map onto through @p view, the head when it is null, failing a check when
 *  the read is refused. */
bool read_strings(fixture_t& fixture, std::span<std::string_view const> texts, read_results_t& results,
                  ustore_view_t view = nullptr) noexcept {
    strided_strings<std::string_view> const strided_texts {{texts.data(), sizeof(std::string_view)}};
    ustore_outcome_t outcome {};
    ustore_strings_read_t read {};
    read.database = fixture.database;
    read.outcome = &outcome;
    read.view = view;
    read.count = texts.size();
    read.collections = {&fixture.collection, 0};
    read.strings = strided_texts.view();
    read.presences = &results.presences;
    read.values = &results.values;
    read.arena = fixture.arena;

    ustore_strings_read(&read);
    bool const read_succeeded = expect_success(outcome);
    return expect_true(texts.empty() || (results.presences && results.values)) && read_succeeded;
}

/** Reads the keys the strings of @p batch map onto, failing a check when the read is refused. */
bool read_batch(fixture_t& fixture, strings_batch_t const& batch, read_results_t& results) noexcept {
    return read_strings(fixture, std::span {batch.texts.data(), batch.count}, results);
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

/** Walks @p prefix from @p start towards @p end in @p direction, at most @p limit strings, failing
 *  a check when the scan is refused. */
bool scan_prefix(fixture_t& fixture, std::string_view prefix, ustore_length_t limit, scan_results_t& results,
                 std::string_view start = {}, ustore_view_t view = nullptr, std::string_view end = {},
                 ustore_scan_direction_t direction = ustore_scan_ascending_k) noexcept {
    strided_strings<std::string_view> const prefixes {{&prefix, 0}};
    strided_strings<std::string_view> const starts {{&start, 0}};
    strided_strings<std::string_view> const ends {{&end, 0}};
    ustore_length_t const* scan_offsets = nullptr;
    ustore_outcome_t outcome {};
    ustore_strings_scan_t scan {};
    scan.database = fixture.database;
    scan.outcome = &outcome;
    scan.view = view;
    scan.count = 1;
    scan.collections = {&fixture.collection, 0};
    scan.prefixes = prefixes.view();
    if (!start.empty()) scan.start_strings = starts.view();
    if (!end.empty()) scan.end_strings = ends.view();
    scan.direction = direction;
    scan.offsets = &scan_offsets;
    scan.strings = &results.strings;
    scan.arena = fixture.arena;
    scan.wanted = limit;

    ustore_strings_scan(&scan);
    bool const scan_succeeded = expect_success(outcome);
    if (!expect_true(scan_offsets != nullptr) || !scan_succeeded) return false;
    results.count = scan_offsets[1] - scan_offsets[0];
    results.strings = tape_slice(results.strings, scan_offsets[0]);
    return true;
}

/** Checks that a scan of @p prefix from @p start towards @p end in @p direction, at most @p limit
 *  strings, returns exactly @p wanted, in that order. */
void expect_scan(fixture_t& fixture, std::string_view prefix, std::span<std::string_view const> wanted,
                 std::string_view start = {}, std::string_view end = {},
                 ustore_scan_direction_t direction = ustore_scan_ascending_k,
                 ustore_length_t limit = batch_capacity_k) noexcept {
    scan_results_t results;
    bool const scanned = scan_prefix(fixture, prefix, limit, results, start, nullptr, end, direction);
    if (!expect_equal(static_cast<std::size_t>(results.count), wanted.size()) || !scanned) return;
    for (std::size_t index = 0; index != wanted.size(); ++index) expect_equal(results[index], wanted[index]);
}

/** Short strings order exactly as their text, whatever byte values they hold. */
void test_strings_short_ordering(config_t const& config) {
    fixture_t fixture {config};
    if (!fixture.ready()) return;

    // Written out of order, including a byte above the signed range, which the key encoding must not reorder
    std::array<std::string_view, 6> const written {"b", "a", "ab", "aa", "\x80", ""};
    strings_batch_t batch;
    for (std::string_view const text : written) batch.assign(text, key_from_integer(batch.count + 1));
    write_batch(fixture, batch);

    std::array<std::string_view, 6> const ordered {"", "a", "aa", "ab", "b", "\x80"};
    expect_scan(fixture, "", ordered);
}

/** A prefix scan returns the members of the prefix, in order, and nothing else. */
void test_strings_prefix_scan(config_t const& config) {
    fixture_t fixture {config};
    if (!fixture.ready()) return;

    strings_batch_t batch;
    batch.assign("user:2", key_from_integer(2));
    batch.assign("team:1", key_from_integer(100));
    batch.assign("user:10", key_from_integer(10));
    batch.assign("user:1", key_from_integer(1));
    write_batch(fixture, batch);

    std::array<std::string_view, 3> const users {"user:1", "user:10", "user:2"};
    expect_scan(fixture, "user:", users);

    std::array<std::string_view, 1> const teams {"team:1"};
    expect_scan(fixture, "team:", teams);

    std::array<std::string_view, 0> const absent {};
    expect_scan(fixture, "missing", absent);
}

/** Strings sharing a long leading run share one head, and each keeps its own key. */
void test_strings_shared_prefix(config_t const& config) {
    fixture_t fixture {config};
    if (!fixture.ready()) return;

    // Fifteen leading bytes in common, so the strings share a key at every key width the engine compiles with
    constexpr std::string_view shared_k = "shared-prefix!!";
    std::array<std::string_view, 3> const written {"shared-prefix!!alpha", "shared-prefix!!gamma",
                                                   "shared-prefix!!beta"};
    strings_batch_t batch;
    batch.assign(written[0], key_from_integer(1));
    batch.assign(written[1], key_from_integer(3));
    batch.assign(written[2], key_from_integer(2));
    write_batch(fixture, batch);

    read_results_t results;
    read_batch(fixture, batch, results);
    for (std::size_t index = 0; index != batch.count; ++index)
        expect_true(results.present(index) && results.value_of(index) == batch.values[index]);

    std::array<std::string_view, 3> const ordered {"shared-prefix!!alpha", "shared-prefix!!beta",
                                                   "shared-prefix!!gamma"};
    expect_scan(fixture, shared_k, ordered);
}

/** The empty string and a string holding a zero byte are ordinary keys. */
void test_strings_empty_and_embedded_zero(config_t const& config) {
    fixture_t fixture {config};
    if (!fixture.ready()) return;

    constexpr std::string_view with_zero_k {"a\0b", 3};
    constexpr std::string_view trailing_zero_k {"a\0", 2};

    // The nil UUID is a key like any other, which only the presence bit tells apart from a missing string
    strings_batch_t batch;
    batch.assign("", key_from_integer(1));
    batch.assign(with_zero_k, ustore_key_t {});
    batch.assign(trailing_zero_k, key_from_integer(3));
    batch.assign("a", key_from_integer(4));
    write_batch(fixture, batch);

    read_results_t results;
    read_batch(fixture, batch, results);
    for (std::size_t index = 0; index != batch.count; ++index)
        expect_true(results.present(index) && results.value_of(index) == batch.values[index]);

    // The length inside the key tells the three strings apart, though their bytes share a run
    std::array<std::string_view, 3> const ordered {"a", trailing_zero_k, with_zero_k};
    expect_scan(fixture, "a", ordered);
}

/** Drops @p collection of the fixture's database as @p mode asks and answers the outcome. */
[[nodiscard]] ustore_outcome_t drop_collection(fixture_t& fixture, ustore_drop_mode_t mode) noexcept {
    ustore_outcome_t outcome {};
    ustore_collection_drop_t drop {};
    drop.database = fixture.database;
    drop.outcome = &outcome;
    drop.collection = fixture.collection;
    drop.mode = mode;
    ustore_collection_drop(&drop);
    return outcome;
}

/** Writing a string again replaces its key, a removal takes it away, and a missing string reads as the nil UUID. A
 *  strings collection holds no values but keys, so dropping its values is refused, while dropping its pairs empties
 *  it. */
void test_strings_update_and_remove(config_t const& config) {
    fixture_t fixture {config};
    if (!fixture.ready()) return;

    constexpr ustore_key_t first_k = key_from_integer(1);
    constexpr ustore_key_t second_k = key_from_integer(2);
    write_one(fixture, "key", first_k);
    strings_batch_t probe;
    probe.assign("key", ustore_key_t {});

    read_results_t results;
    read_batch(fixture, probe, results);
    expect_true(results.present(0) && results.value_of(0) == first_k);

    write_one(fixture, "key", second_k);
    read_batch(fixture, probe, results);
    expect_true(results.present(0) && results.value_of(0) == second_k);

    ustore_outcome_t refused = drop_collection(fixture, ustore_drop_values_k);
    expect_status(status_t {refused}, status_code_t::invalid_argument_k);
    read_batch(fixture, probe, results);
    expect_true(results.present(0) && results.value_of(0) == second_k);

    erase_one(fixture, "key");
    read_batch(fixture, probe, results);
    expect_true(!results.present(0));
    expect_equal(results.value_of(0), ustore_key_t {});

    write_one(fixture, "key", first_k);
    ustore_outcome_t emptied = drop_collection(fixture, ustore_drop_pairs_k);
    expect_success(emptied);
    read_batch(fixture, probe, results);
    expect_true(!results.present(0));

    std::array<std::string_view, 0> const empty {};
    expect_scan(fixture, "", empty);
}

/** Checks that a scan of @p prefix through @p view, the head when it is null, lists exactly @p
 *  wanted, in that order, reporting only the first string out of place. */
void expect_listed(fixture_t& fixture, std::string_view prefix, std::span<std::string_view const> wanted,
                   ustore_view_t view = nullptr) noexcept {
    scan_results_t results;
    bool const scanned = scan_prefix(fixture, prefix, static_cast<ustore_length_t>(wanted.size() + 1), results, {},
                                     view);
    if (!expect_equal(static_cast<std::size_t>(results.count), wanted.size()) || !scanned) return;
    for (std::size_t index = 0; index != wanted.size(); ++index)
        if (!expect_equal(results[index], wanted[index])) return;
}

/** Ten thousand strings sharing their leading 15 bytes, so every head ties, each map onto a distinct UUID, and a read
 *  and a walk find each, also once half of them are removed; a batch naming one string twice keeps its last write. */
void test_strings_shared_heads(config_t const& config) {
    fixture_t fixture {config};
    if (!fixture.ready()) return;

    constexpr std::size_t count_k = 10'000;
    constexpr std::size_t batch_k = 1'000;
    constexpr std::string_view shared_k = "shared-prefix!!";
    std::vector<std::string> texts(count_k);
    std::vector<std::string_view> text_views(count_k);
    std::vector<ustore_key_t> values(count_k);
    for (std::size_t index = 0; index != count_k; ++index) {
        texts[index] = fmt::format("{}{:05}", shared_k, index);
        text_views[index] = texts[index];
        values[index] = key_from_integer(index + 1);
    }
    std::span<std::string_view const> const all_texts {text_views};
    std::span<ustore_key_t const> const all_values {values};
    for (std::size_t first = 0; first != count_k; first += batch_k) {
        ustore_outcome_t outcome = write_strings(fixture, all_texts.subspan(first, batch_k),
                                                 all_values.subspan(first, batch_k));
        expect_success(outcome);
    }
    bool held = true;
    for (std::size_t first = 0; first != count_k && held; first += batch_k) {
        read_results_t results;
        held = read_strings(fixture, all_texts.subspan(first, batch_k), results);
        for (std::size_t index = 0; index != batch_k && held; ++index)
            held = expect_true(results.present(index) && results.value_of(index) == values[first + index]);
    }
    expect_listed(fixture, shared_k, all_texts);

    std::array<std::string_view, 3> const twice {text_views[0], text_views[1], text_views[0]};
    std::array<ustore_key_t, 3> const rewrites {key_from_integer(count_k + 1), key_from_integer(count_k + 2),
                                                key_from_integer(count_k + 3)};
    ustore_outcome_t rewritten = write_strings(fixture, twice, rewrites);
    expect_success(rewritten);
    read_results_t results;
    read_strings(fixture, std::span {twice}.first(2), results);
    expect_true(results.present(0) && results.value_of(0) == rewrites[2]);
    expect_true(results.present(1) && results.value_of(1) == rewrites[1]);

    // Every odd string goes, and the even ones stay listed, in order
    std::vector<std::string_view> odd;
    std::vector<std::string_view> even;
    for (std::size_t index = 0; index != count_k; ++index) (index % 2 ? odd : even).push_back(text_views[index]);
    erase_strings(fixture, odd);
    expect_listed(fixture, shared_k, even);
    read_strings(fixture, std::span {odd}.first(2), results);
    expect_true(!results.present(0) && !results.present(1));
}

/** Creates a collection holding @p modality under @p name in the fixture's database into @p collection and answers
 *  the outcome. */
[[nodiscard]] ustore_outcome_t create_named(fixture_t& fixture, std::string_view name, ustore_modality_t modality,
                                            ustore_collection_id_t& collection) noexcept {
    ustore_outcome_t outcome {};
    ustore_collection_create_t create {};
    create.database = fixture.database;
    create.outcome = &outcome;
    create.name = bytes_of(name);
    create.modality = modality;
    create.collection = &collection;
    ustore_collection_create(&create);
    return outcome;
}

/** Every modality shares one namespace of collection names, so a name a blobs collection
 *  holds is refused for a strings collection, and a name a strings collection holds is
 *  refused for a blobs collection. */
void test_strings_name_taken_across_modalities(config_t const& config) {
    fixture_t fixture {config};
    if (!fixture.ready()) return;

    ustore_collection_id_t blobs {};
    ustore_outcome_t outcome = create_named(fixture, "taken.by.blobs", ustore_modality_blobs_k, blobs);
    expect_success(outcome);
    ustore_collection_id_t strings {};
    outcome = create_named(fixture, "taken.by.blobs", ustore_modality_strings_k, strings);
    expect_status(status_t {outcome}, status_code_t::already_exists_k);
    expect_true(strings == ustore_collection_id_t {});

    outcome = create_named(fixture, "taken.by.strings", ustore_modality_strings_k, strings);
    expect_success(outcome);
    outcome = create_named(fixture, "taken.by.strings", ustore_modality_blobs_k, blobs);
    expect_status(status_t {outcome}, status_code_t::already_exists_k);
    expect_true(blobs == ustore_collection_id_t {});
}

/** Two transactions writing two long strings of one head both commit, as every string is watched on its own, while of
 *  two transactions writing one string the later commit fails. */
case_result_t test_strings_one_head_two_writers(config_t const& config) {
    fixture_t fixture {config};
    if (!fixture.ready()) return {};
    ustore_outcome_t described {};
    ustore_database_description_t description {};
    ustore_database_describe_t describe {};
    describe.database = fixture.database;
    describe.outcome = &described;
    describe.description = &description;
    ustore_database_describe(&describe);
    if (!expect_success(described)) return {};
    if (!(description.capabilities & ustore_capabilities_transactions_k))
        return case_result_t::skipped(needs_transactions_k);

    std::string const first_text = std::string {"shared-prefix!!"} + std::string(40, 'a');
    std::string const second_text = std::string {"shared-prefix!!"} + std::string(40, 'b');
    std::array<std::string_view, 1> const first {first_text};
    std::array<std::string_view, 1> const second {second_text};
    std::array<ustore_key_t, 1> const first_keys {key_from_integer(1)};
    std::array<ustore_key_t, 1> const second_keys {key_from_integer(2)};

    // Each transaction reads its string before writing it, so the read is watched as well as the write
    transaction_holder_t first_writer;
    transaction_holder_t second_writer;
    if (!begin_transaction(fixture, first_writer) || !begin_transaction(fixture, second_writer)) return {};
    read_results_t results;
    read_strings(fixture, first, results, ustore_transaction_view(first_writer.transaction, ustore_watch_keys_k));
    expect_true(!results.present(0));
    read_strings(fixture, second, results, ustore_transaction_view(second_writer.transaction, ustore_watch_keys_k));
    expect_true(!results.present(0));
    ustore_outcome_t outcome = write_strings(fixture, first, first_keys, first_writer.transaction);
    expect_success(outcome);
    outcome = write_strings(fixture, second, second_keys, second_writer.transaction);
    expect_success(outcome);
    // A walk through a transaction lists its own writes, and nothing another transaction has not committed
    expect_listed(fixture, "shared-prefix!!", first,
                  ustore_transaction_view(first_writer.transaction, ustore_watch_keys_k));
    outcome = commit_transaction(fixture, first_writer.transaction);
    expect_success(outcome);
    outcome = commit_transaction(fixture, second_writer.transaction);
    expect_success(outcome);

    std::array<std::string_view, 2> const both {first_text, second_text};
    read_strings(fixture, both, results);
    expect_true(results.present(0) && results.value_of(0) == first_keys[0]);
    expect_true(results.present(1) && results.value_of(1) == second_keys[0]);

    transaction_holder_t earlier;
    transaction_holder_t later;
    if (!begin_transaction(fixture, earlier) || !begin_transaction(fixture, later)) return {};
    outcome = write_strings(fixture, first, second_keys, later.transaction);
    expect_success(outcome);
    outcome = write_strings(fixture, first, first_keys, earlier.transaction);
    expect_success(outcome);
    outcome = commit_transaction(fixture, earlier.transaction);
    expect_success(outcome);
    outcome = commit_transaction(fixture, later.transaction);
    expect_status(outcome, status_code_t::write_conflict_k);
    read_strings(fixture, first, results);
    expect_true(results.present(0) && results.value_of(0) == first_keys[0]);
    return {};
}

/** A read through a transaction finds the strings it wrote and those committed before it began,
 *  and not one another commit wrote after it began. */
case_result_t test_strings_transaction_reads_its_own(config_t const& config) {
    fixture_t fixture {config};
    if (!fixture.ready()) return {};
    ustore_outcome_t described {};
    ustore_database_description_t description {};
    ustore_database_describe_t describe {};
    describe.database = fixture.database;
    describe.outcome = &described;
    describe.description = &description;
    ustore_database_describe(&describe);
    if (!expect_success(described)) return {};
    if (!(description.capabilities & ustore_capabilities_transactions_k))
        return case_result_t::skipped(needs_transactions_k);

    std::array<std::string_view, 3> const texts {"before", "own", "after"};
    std::array<ustore_key_t, 3> const keys {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
    write_one(fixture, texts[0], keys[0]);
    transaction_holder_t reader;
    if (!begin_transaction(fixture, reader)) return {};
    ustore_outcome_t outcome = write_strings(fixture, std::span {&texts[1], 1}, std::span {&keys[1], 1},
                                             reader.transaction);
    expect_success(outcome);
    write_one(fixture, texts[2], keys[2]);

    read_results_t results;
    read_strings(fixture, texts, results, ustore_transaction_view(reader.transaction, ustore_watch_keys_k));
    expect_true(results.present(0) && results.value_of(0) == keys[0]);
    expect_true(results.present(1) && results.value_of(1) == keys[1]);
    expect_true(!results.present(2));
    return {};
}

/** A string of 64 KiB is a key like any other, beside a shorter one of the same head: it reads
 *  back, a walk of a prefix lists it whole, and a removal takes it away. */
void test_strings_long_string(config_t const& config) {
    fixture_t fixture {config};
    if (!fixture.ready()) return;

    std::string long_text(64 * 1024, '\0');
    for (std::size_t index = 0; index != long_text.size(); ++index)
        long_text[index] = static_cast<char>('a' + index % 26);
    std::string const short_text = long_text.substr(0, 20);
    std::array<std::string_view, 2> const texts {long_text, short_text};
    std::array<ustore_key_t, 2> const values {key_from_integer(1), key_from_integer(2)};
    ustore_outcome_t outcome = write_strings(fixture, texts, values);
    expect_success(outcome);

    read_results_t results;
    read_strings(fixture, texts, results);
    expect_true(results.present(0) && results.value_of(0) == values[0]);
    expect_true(results.present(1) && results.value_of(1) == values[1]);

    // A prefix within the head lists both, the shorter first, and a prefix far past the head the long one alone
    std::array<std::string_view, 2> const ordered {short_text, long_text};
    expect_listed(fixture, std::string_view {long_text}.substr(0, 10), ordered);
    std::array<std::string_view, 1> const alone {long_text};
    expect_listed(fixture, std::string_view {long_text}.substr(0, 40 * 1024), alone);

    erase_one(fixture, long_text);
    read_strings(fixture, texts, results);
    expect_true(!results.present(0));
    expect_true(results.present(1) && results.value_of(1) == values[1]);
    std::array<std::string_view, 1> const left {short_text};
    expect_listed(fixture, std::string_view {long_text}.substr(0, 10), left);
}

/** A scan stops at its limit and resumes from the string it is given. */
void test_strings_scan_pagination(config_t const& config) {
    fixture_t fixture {config};
    if (!fixture.ready()) return;

    strings_batch_t batch;
    batch.assign("page:a", key_from_integer(1));
    batch.assign("page:b", key_from_integer(2));
    batch.assign("page:c", key_from_integer(3));
    write_batch(fixture, batch);

    scan_results_t first_page;
    scan_prefix(fixture, "page:", 2, first_page);
    if (expect_equal(static_cast<std::size_t>(first_page.count), 2u)) {
        expect_equal(first_page[0], std::string_view("page:a"));
        expect_equal(first_page[1], std::string_view("page:b"));
    }

    scan_results_t second_page;
    if (scan_prefix(fixture, "page:", 8, second_page, "page:c") &&
        expect_equal(static_cast<std::size_t>(second_page.count), 1u))
        expect_equal(second_page[0], std::string_view("page:c"));
}

#pragma region Bounded Ranges

/** The strings every range case writes, in order, each mapping onto the key of its position. */
constexpr std::array<std::string_view, 6> range_strings_k {"a", "ab", "abc", "b", "ba", "c"};

/** Writes @c range_strings_k into @p fixture. */
bool write_range_strings(fixture_t& fixture) noexcept {
    strings_batch_t batch;
    for (std::size_t index = 0; index != range_strings_k.size(); ++index)
        batch.assign(range_strings_k[index], key_from_integer(index));
    return write_batch(fixture, batch);
}

/** A scan stops before its end string, which an empty one leaves unbounded, inside its prefix. */
void test_strings_range_end_bound(config_t const& config) {
    fixture_t tested {config};
    fixture_t reference {abi::open_reference<ustore_database_t>(config)};
    if (!tested.ready() || !reference.ready()) return;
    for (fixture_t* fixture : {&tested, &reference}) {
        write_range_strings(*fixture);
        std::array<std::string_view, 2> const middle {"ab", "abc"};
        expect_scan(*fixture, "", middle, "ab", "b");
        std::array<std::string_view, 2> const prefixed {"a", "ab"};
        expect_scan(*fixture, "a", prefixed, {}, "abc");
        std::array<std::string_view, 3> const unbounded {"b", "ba", "c"};
        expect_scan(*fixture, "", unbounded, "b", {});
        expect_scan(*fixture, "", {}, "b", "ab");
    }
}

/** A descending scan lists from its start string down to past its end string, or from the last
 *  string of its prefix when it names no start. */
void test_strings_range_descending(config_t const& config) {
    fixture_t tested {config};
    fixture_t reference {abi::open_reference<ustore_database_t>(config)};
    if (!tested.ready() || !reference.ready()) return;
    for (fixture_t* fixture : {&tested, &reference}) {
        write_range_strings(*fixture);
        std::array<std::string_view, 4> const bounded {"ba", "b", "abc", "ab"};
        expect_scan(*fixture, "", bounded, "ba", "a", ustore_scan_descending_k);
        std::array<std::string_view, 3> const prefixed {"abc", "ab", "a"};
        expect_scan(*fixture, "a", prefixed, {}, {}, ustore_scan_descending_k);
        std::array<std::string_view, 2> const limited {"c", "ba"};
        expect_scan(*fixture, "", limited, {}, {}, ustore_scan_descending_k, 2);
    }
}

/** A descending cursor with an end string pages down from its start, stopping before the end. */
void test_strings_cursor_descending_end(config_t const& config) {
    fixture_t tested {config};
    fixture_t reference {abi::open_reference<ustore_database_t>(config)};
    if (!tested.ready() || !reference.ready()) return;
    std::array<std::string_view, 4> const pinned {"c", "ba", "b", "abc"};
    for (fixture_t* fixture : {&tested, &reference}) {
        write_range_strings(*fixture);
        ustore_strings_cursor_t cursor = nullptr;
        ustore_outcome_t outcome {};
        ustore_strings_cursor_init_t init {};
        init.cursor = &cursor;
        init.database = fixture->database;
        init.outcome = &outcome;
        init.collection = fixture->collection;
        init.start_string = bytes_of("c");
        init.end_string = bytes_of("ab");
        init.direction = ustore_scan_descending_k;
        ustore_strings_cursor_init(&init);
        if (!expect_success(outcome)) continue;
        std::size_t first = 0;
        for (std::size_t const count : {2, 2, 0}) {
            ustore_strings_view_t found {};
            ustore_key_t const* values = nullptr;
            ustore_size_t written = 0;
            ustore_strings_cursor_next_t next {};
            next.database = fixture->database;
            next.outcome = &outcome;
            next.cursor = cursor;
            next.strings = &found;
            next.values = &values;
            next.written = &written;
            next.arena = fixture->arena;
            next.wanted = 2;
            ustore_strings_cursor_next(&next);
            expect_success(outcome);
            expect_equal(static_cast<std::size_t>(written), count);
            for (std::size_t index = 0; index != written && index != count; ++index)
                expect_equal(string_of(tape_string(found, index)), pinned[first + index]);
            first += count;
        }
        ustore_strings_cursor_free(cursor);
    }
}

/** A range removal removes every string from each start string up to its end string, or to the last
 *  string past an empty one. */
void test_strings_range_remove(config_t const& config) {
    fixture_t tested {config};
    fixture_t reference {abi::open_reference<ustore_database_t>(config)};
    if (!tested.ready() || !reference.ready()) return;
    std::array<std::string_view, 2> const starts {"ab", "c"};
    std::array<std::string_view, 2> const ends {"b", ""};
    strided_strings<std::string_view> const strided_starts {{starts.data(), sizeof(std::string_view)}};
    strided_strings<std::string_view> const strided_ends {{ends.data(), sizeof(std::string_view)}};
    for (fixture_t* fixture : {&tested, &reference}) {
        write_range_strings(*fixture);
        ustore_outcome_t outcome {};
        ustore_strings_remove_t remove {};
        remove.database = fixture->database;
        remove.outcome = &outcome;
        remove.count = starts.size();
        remove.collections = {&fixture->collection, 0};
        remove.strings = strided_starts.view();
        remove.end_strings = strided_ends.view();
        ustore_strings_remove(&remove);
        expect_success(outcome);
        std::array<std::string_view, 3> const remaining {"a", "b", "ba"};
        expect_scan(*fixture, "", remaining);
    }
}

/** A measure counts the strings of its range, each holding the key it maps onto, which the engine
 *  under test bounds around the exact count of the reference. */
void test_strings_range_measure(config_t const& config) {
    fixture_t tested {config};
    fixture_t reference {abi::open_reference<ustore_database_t>(config)};
    if (!tested.ready() || !reference.ready()) return;
    struct measured_range_t {
        std::string_view start;
        std::string_view end;
        ustore_scan_direction_t direction;
        std::size_t cardinality;
    };
    std::array<measured_range_t, 3> const ranges {measured_range_t {"ab", "b", ustore_scan_ascending_k, 2},
                                                  measured_range_t {"ba", "a", ustore_scan_descending_k, 4},
                                                  measured_range_t {{}, {}, ustore_scan_ascending_k, 6}};
    for (fixture_t* fixture : {&tested, &reference}) {
        write_range_strings(*fixture);
        for (measured_range_t const& range : ranges) {
            strided_strings<std::string_view> const starts {{&range.start, 0}};
            strided_strings<std::string_view> const ends {{&range.end, 0}};
            ustore_size_t const *min_cardinalities = nullptr, *max_cardinalities = nullptr;
            ustore_size_t const *min_value_bytes = nullptr, *max_value_bytes = nullptr;
            ustore_outcome_t outcome {};
            ustore_strings_measure_t measure {};
            measure.database = fixture->database;
            measure.outcome = &outcome;
            measure.count = 1;
            measure.collections = {&fixture->collection, 0};
            if (!range.start.empty()) measure.start_strings = starts.view();
            if (!range.end.empty()) measure.end_strings = ends.view();
            measure.direction = range.direction;
            measure.min_cardinalities = &min_cardinalities;
            measure.max_cardinalities = &max_cardinalities;
            measure.min_value_bytes = &min_value_bytes;
            measure.max_value_bytes = &max_value_bytes;
            measure.arena = fixture->arena;
            ustore_strings_measure(&measure);
            if (!expect_success(outcome)) continue;
            if (!expect_true(min_cardinalities && max_cardinalities && min_value_bytes && max_value_bytes)) continue;
            std::size_t const key_bytes = range.cardinality * sizeof(ustore_key_t);
            expect_true(min_cardinalities[0] <= range.cardinality && range.cardinality <= max_cardinalities[0]);
            expect_true(min_value_bytes[0] <= key_bytes && key_bytes <= max_value_bytes[0]);
            if (fixture == &reference) expect_equal(min_cardinalities[0], max_cardinalities[0]);
        }
    }
}

/** A sample draws distinct written strings, as many as asked for or every one when fewer exist. */
void test_strings_sample(config_t const& config) {
    fixture_t tested {config};
    fixture_t reference {abi::open_reference<ustore_database_t>(config)};
    if (!tested.ready() || !reference.ready()) return;
    for (fixture_t* fixture : {&tested, &reference}) {
        write_range_strings(*fixture);
        for (std::size_t const wanted : {4, 10}) {
            ustore_length_t const* offsets = nullptr;
            ustore_strings_view_t drawn {};
            ustore_outcome_t outcome {};
            ustore_strings_sample_t sample {};
            sample.database = fixture->database;
            sample.outcome = &outcome;
            sample.count = 1;
            sample.collections = {&fixture->collection, 0};
            sample.offsets = &offsets;
            sample.strings = &drawn;
            sample.arena = fixture->arena;
            sample.wanted = wanted;
            ustore_strings_sample(&sample);
            if (!expect_success(outcome)) continue;
            if (!expect_true(offsets != nullptr)) continue;
            std::size_t const count = offsets[1] - offsets[0];
            expect_equal(count, wanted < range_strings_k.size() ? wanted : range_strings_k.size());
            for (std::size_t index = 0; index != count; ++index) {
                std::string_view const text = string_of(tape_string(drawn, offsets[0] + index));
                expect_true(std::find(range_strings_k.begin(), range_strings_k.end(), text) != range_strings_k.end());
                for (std::size_t earlier = 0; earlier != index; ++earlier)
                    expect_true(string_of(tape_string(drawn, offsets[0] + earlier)) != text);
            }
        }
    }
}

#pragma endregion Bounded Ranges

} // namespace unum::ustore::test

int main(int argc, char** argv) {
    start_test_run(argc, argv);

    auto const maybe_test_directory = scratch_directory_t::make(test_run.settings.directory);
    if (!maybe_test_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", test_run.settings.directory);
        return 1;
    }
    char const* const directory = maybe_test_directory->path();
    if (directory) fmt::println("Will work in directory: {}", directory);
    else fmt::println("Will work with default configuration");
    config_t const config {test_run.settings.engine, directory};

    run_test_case(engine_label_t {"test_strings_short_ordering"}.c_str(), test_strings_short_ordering, config);
    run_test_case(engine_label_t {"test_strings_prefix_scan"}.c_str(), test_strings_prefix_scan, config);
    run_test_case(engine_label_t {"test_strings_shared_prefix"}.c_str(), test_strings_shared_prefix, config);
    run_test_case(engine_label_t {"test_strings_empty_and_embedded_zero"}.c_str(), test_strings_empty_and_embedded_zero,
                  config);
    run_test_case(engine_label_t {"test_strings_update_and_remove"}.c_str(), test_strings_update_and_remove, config);
    run_test_case(engine_label_t {"test_strings_shared_heads"}.c_str(), test_strings_shared_heads, config);
    run_test_case(engine_label_t {"test_strings_name_taken_across_modalities"}.c_str(),
                  test_strings_name_taken_across_modalities, config);
    run_test_case(engine_label_t {"test_strings_one_head_two_writers"}.c_str(), test_strings_one_head_two_writers,
                  config);
    run_test_case(engine_label_t {"test_strings_transaction_reads_its_own"}.c_str(),
                  test_strings_transaction_reads_its_own, config);
    run_test_case(engine_label_t {"test_strings_long_string"}.c_str(), test_strings_long_string, config);
    run_test_case(engine_label_t {"test_strings_scan_pagination"}.c_str(), test_strings_scan_pagination, config);
    run_test_case(engine_label_t {"test_strings_range_end_bound"}.c_str(), test_strings_range_end_bound, config);
    run_test_case(engine_label_t {"test_strings_range_descending"}.c_str(), test_strings_range_descending, config);
    run_test_case(engine_label_t {"test_strings_cursor_descending_end"}.c_str(), test_strings_cursor_descending_end,
                  config);
    run_test_case(engine_label_t {"test_strings_range_remove"}.c_str(), test_strings_range_remove, config);
    run_test_case(engine_label_t {"test_strings_range_measure"}.c_str(), test_strings_range_measure, config);
    run_test_case(engine_label_t {"test_strings_sample"}.c_str(), test_strings_sample, config);

    return finish_test_run();
}
