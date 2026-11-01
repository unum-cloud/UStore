/**
 *  @file test/abi/database.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Checks that the C ABI accepts valid argument combinations and rejects malformed ones,
 *      from writes, reads, transactions and scans to saves, compactions and their progress.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`, `std::uintptr_t`
#include <cstdio>  // `stderr`

#include <algorithm>    // `std::equal`
#include <array>        // `std::array`
#include <chrono>       // `std::chrono::milliseconds`
#include <filesystem>   // `std::filesystem::exists`, `std::filesystem::is_empty`, `std::filesystem::remove_all`
#include <limits>       // `std::numeric_limits`
#include <span>         // `std::span`
#include <string>       // `std::string`
#include <string_view>  // `std::string_view`
#include <system_error> // `std::error_code`
#include <thread>       // `std::this_thread::sleep_for`
#include <utility>      // `std::move`

#include <fmt/format.h>

#include "ustore/ustore.hpp"

#include "abi/fixture.hpp" // `write_text`, `create_collection`, `read_of`, `open_cursor`, `pull_of`, `expect_page`
#include "harness.hpp"     // `suite_exit_code`, `expect`, `expect_failure`, `expect_status`, `open_engine`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::test::abi;
using namespace unum::ustore::utilities;
using namespace unum;

/** Every engine refuses an init without an output, and all but the in-memory one refuse to open
 *  without a directory. */
std::size_t database_init_refusals(config_t const& config) {
    std::size_t failures = 0;
    ustore_outcome_t outcome {};
    open_engine(config.options(), nullptr, &outcome);
    failures += expect_failure(outcome);

    engine_options_t without_directory = config.options();
    without_directory.directory = nullptr;
    ustore_database_t database = nullptr;
    open_engine(without_directory, &database, &outcome);
    failures += config.engine() == "ram" ? expect_success(outcome) : expect_failure(outcome);
    ustore_database_free(database);
    return failures;
}

/** Writes, reads, transactions and scans accept valid tasks and reject malformed ones,
 *  per engine capability. */
std::size_t database_validation(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "database_validation.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    expected<transaction_t> maybe_transaction = database.transaction();
    ustore_transaction_t const transaction = maybe_transaction ? maybe_transaction->transaction() : nullptr;
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());

    constexpr ustore_length_t count = 3;
    std::array<ustore_key_t, count> keys {key_from_integer(34), key_from_integer(35), key_from_integer(36)};
    std::array<std::uint64_t, count> numbers {34, 35, 36};
    constexpr std::uint32_t number_length = sizeof(std::uint64_t);
    std::array<std::uint32_t, count + 1> number_offsets {0, number_length, number_length * 2, number_length * 3};
    ustore_outcome_t outcome {};

#pragma region Writes

    // Every write below varies one field of this valid one, which writes the numbers as one tape
    ustore_blobs_write_t const write_head {
        .database = database,
        .outcome = &outcome,
        .count = count,
        .collections = {&blobs, 0},
        .keys = {keys.data(), sizeof(ustore_key_t)},
        .values = u32_tape(numbers.data(), number_offsets.data()),
    };
    for (ustore_durability_t const durability :
         std::array {ustore_durability_buffered_k, ustore_durability_flushed_k}) {
        ustore_blobs_write_t write = write_head;
        write.durability = durability;
        ustore_blobs_write(&write);
        failures += expect_success(outcome);
    }

    // Collections nobody created, the nil UUID among them
    std::array<ustore_collection_id_t, count> collections {
        {{uuid_from_integer(1)}, {uuid_from_integer(2)}, {uuid_from_integer(3)}}};
    ustore_blobs_write_t write_unnamed = write_head;
    write_unnamed.collections = {collections.data(), sizeof(ustore_collection_id_t)};
    ustore_blobs_write(&write_unnamed);
    failures += expect_failure(outcome);

    ustore_collection_id_t const nil_collection {};
    ustore_blobs_write_t write_nil = write_head;
    write_nil.collections = {&nil_collection, 0};
    ustore_blobs_write(&write_nil);
    failures += expect_failure(outcome);

    // No collection at all
    ustore_blobs_write_t write_null_collection = write_head;
    write_null_collection.collections = {};
    ustore_blobs_write(&write_null_collection);
    failures += expect_status(outcome, status_code_t::invalid_argument_k);

    if (transaction) {
        ustore_blobs_write_t write = write_head;
        write.transaction = transaction;
        ustore_blobs_write(&write);
        failures += expect_success(outcome);

        // A transaction flushes at commit, not at a write
        write.durability = ustore_durability_flushed_k;
        ustore_blobs_write(&write);
        failures += expect_failure(outcome);
    }

    // No tasks, yet keys are given
    ustore_blobs_write_t write_no_tasks = write_head;
    write_no_tasks.count = 0;
    ustore_blobs_write(&write_no_tasks);
    failures += expect_failure(outcome);

    // Tasks without keys
    ustore_blobs_write_t write_null_keys = write_head;
    write_null_keys.keys = {};
    ustore_blobs_write(&write_null_keys);
    failures += expect_failure(outcome);

    // Tasks without values, which no default stands for
    ustore_blobs_write_t write_null_values = write_head;
    write_null_values.values = {};
    ustore_blobs_write(&write_null_values);
    failures += expect_failure(outcome);

    // Keys stay unwatched only inside a transaction
    ustore_blobs_write_t write_blind_head = write_head;
    write_blind_head.watch = ustore_watch_none_k;
    ustore_blobs_write(&write_blind_head);
    failures += expect_failure(outcome);

    // A strings write checks its durability as every other write does
    auto [strings, strings_status] = create_collection(database, "database_validation.strings",
                                                       ustore_modality_strings_k);
    failures += expect(strings_status.succeeded());
    std::array<std::uint32_t, 2> const string_offsets {0, 5};
    ustore_strings_write_t write_strings {
        .database = database,
        .outcome = &outcome,
        .count = 1,
        .collections = {&strings, 0},
        .strings = u32_tape("apple", string_offsets.data()),
        .values = {keys.data(), sizeof(ustore_key_t)},
        .durability = static_cast<ustore_durability_t>(7),
    };
    ustore_strings_write(&write_strings);
    failures += expect_status(outcome, status_code_t::invalid_argument_k);

#pragma endregion Writes

#pragma region Reads

    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, keys, columns, arena, outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    if (tape_written(columns.values)) failures += expect_eq(columns.value(2).size(), sizeof(std::uint64_t));

    if (transaction) {
        read.view = ustore_transaction_view(transaction, ustore_watch_none_k);
        ustore_blobs_read(&read);
        failures += expect_success(outcome);
    }

    // A gist names its documents by their keys
    auto [docs, docs_status] = create_collection(database, "database_validation.docs", ustore_modality_docs_k);
    failures += expect(docs_status.succeeded());
    ustore_strings_view_t gisted {};
    ustore_docs_gist_t gist_without_keys {
        .database = database,
        .outcome = &outcome,
        .count = 1,
        .collections = {&docs, 0},
        .fields = &gisted,
        .arena = arena,
    };
    ustore_docs_gist(&gist_without_keys);
    failures += expect_status(outcome, status_code_t::invalid_argument_k);

#pragma endregion Reads

#pragma region Transactions

    ustore_transaction_t raw_transaction = nullptr;
    ustore_transaction_init_t transaction_init {
        .transaction = &raw_transaction,
        .database = database,
        .outcome = &outcome,
    };
    ustore_transaction_init(&transaction_init);
    failures += expect((outcome.status == ustore_status_success_k) == database.supports_transactions());
    outcome = {};

    transaction_init.transaction = nullptr;
    ustore_transaction_init(&transaction_init);
    failures += expect_failure(outcome);

    // A commit needs the transaction it publishes
    ustore_transaction_commit_t transaction_commit {
        .database = database,
        .outcome = &outcome,
    };
    ustore_transaction_commit(&transaction_commit);
    failures += expect_failure(outcome);
    ustore_transaction_free(raw_transaction);

#pragma endregion Transactions

#pragma region Scans

    ustore_length_t const* scan_offsets = nullptr;
    ustore_key_t const* found_keys = nullptr;
    ustore_blobs_scan_t scan {
        .database = database,
        .outcome = &outcome,
        .view = ustore_transaction_view(transaction, ustore_watch_keys_k),
        .count = 1,
        .collections = {&blobs, 0},
        .start_keys = {keys.data(), 0},
        .offsets = &scan_offsets,
        .keys = &found_keys,
        .arena = arena,
        .wanted = count,
    };
    ustore_blobs_scan(&scan);
    failures += expect_success(outcome);

    // Without start keys a scan starts at the first key
    ustore_blobs_scan_t scan_without_start_keys = scan;
    scan_without_start_keys.start_keys = {};
    ustore_blobs_scan(&scan_without_start_keys);
    failures += expect_success(outcome);

    ustore_blobs_scan_t scan_without_keys_output = scan;
    scan_without_keys_output.keys = nullptr;
    ustore_blobs_scan(&scan_without_keys_output);
    failures += expect_failure(outcome);

#pragma endregion Scans

    return failures;
}

/** A pointer neither view constructor made, a raw handle with a stray bit above the tag or a bare
 *  transaction, is refused before any engine reads through it. */
std::size_t database_forged_views(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    auto [blobs, blobs_status] = create_collection(database, "database_forged_views.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());

    // The read names an arena and a collection, so the view, not a missing input, is what
    // the router refuses
    ustore_key_t const key = key_from_integer(1);
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read {};
    read.database = database;
    read.outcome = &outcome;
    read.count = 1;
    read.collections = {&blobs, 0};
    read.keys = {&key, 0};
    read.arena = arena;

    // Handles align past the two tag bits, so the next bit up leaves the tag empty while the
    // pointer stays set
    std::uintptr_t const handle_bits = reinterpret_cast<std::uintptr_t>(static_cast<ustore_database_t>(database));
    read.view = reinterpret_cast<ustore_view_t>(handle_bits | 4u);
    ustore_blobs_read(&read);
    failures += expect_status(outcome, status_code_t::invalid_argument_k);

    expected<transaction_t> maybe_transaction = database.transaction();
    if (!maybe_transaction) return failures;
    read.view = reinterpret_cast<ustore_view_t>(maybe_transaction->transaction());
    ustore_blobs_read(&read);
    failures += expect_status(outcome, status_code_t::invalid_argument_k);
    return failures;
}

/** A view or a transaction of another engine is refused before either engine sees it. */
case_result_t database_foreign_handles(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;

    // The in-memory engine needs no directory, so it is the other engine beside every disk engine
    database_t other;
    if (config.engine() != "ram") {
        engine_options_t ram = config.options();
        ram.name = "ram";
        auto [opened, other_status] = open_database<database_t>(config_t {ram, nullptr});
        failures += expect(other_status.succeeded());
        other = std::move(opened);
    }
    else if (config.directory()) {
        path_t const directory {config.directory(), "other"};
        failures += expect(recreate_directory(directory.c_str()));
        for (std::string_view const name : std::array<std::string_view, 2> {"rocksdb", "leveldb"}) {
            engine_options_t disk = config.options();
            disk.name = name;
            if (auto opened = open_database<database_t>(config_t {disk, directory.c_str()}); opened) {
                other = *std::move(opened);
                break;
            }
        }
    }
    else return case_result_t::skipped(needs_directory_k, failures);
    if (!other) return case_result_t::skipped("only one engine is built", failures);

    expected<transaction_t> maybe_transaction = other.transaction();
    expected<snapshot_t> maybe_snapshot = other.snapshot();
    // A transaction is tried first, since it is refused both as a view and as the transaction
    // of a write
    ustore_view_t foreign = nullptr;
    if (maybe_transaction) foreign = ustore_transaction_view(maybe_transaction->transaction(), ustore_watch_keys_k);
    else if (maybe_snapshot) foreign = ustore_snapshot_view(maybe_snapshot->snapshot());
    else return case_result_t::skipped("the other engine hands out no views", failures);

    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    auto [blobs, blobs_status] = create_collection(database, "database_foreign_handles.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    ustore_key_t const key = key_from_integer(1);
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read {};
    read.database = database;
    read.outcome = &outcome;
    read.view = foreign;
    read.count = 1;
    read.collections = {&blobs, 0};
    read.keys = {&key, 0};
    read.arena = arena;
    ustore_blobs_read(&read);
    failures += expect_status(outcome, status_code_t::invalid_argument_k);
    if (!maybe_transaction) return failures;

    ustore_blobs_write_t write {};
    write.database = database;
    write.outcome = &outcome;
    write.transaction = maybe_transaction->transaction();
    write.count = 1;
    write.collections = {&blobs, 0};
    write.keys = {&key, 0};
    write.values = stored_values_k.view();
    ustore_blobs_write(&write);
    failures += expect_status(outcome, status_code_t::invalid_argument_k);
    return failures;
}

/** A call that fails zeroes the scalars it reports and leaves the handles it makes @c NULL,
 *  whatever they held: a description, a snapshot and its stamp, a listing's count, a collection,
 *  the stamps of a stage and of a commit, a gist's count, a transaction and an executor. */
std::size_t database_failed_reports_zeroed(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    ustore_outcome_t outcome {};
    std::uintptr_t const stale_handle = 0x40;
    ustore_stamp_t const stale_stamp = 77;

    // Without a database every call is uninitialized, which only the router sees
    ustore_database_description_t description {.capabilities = ustore_capabilities_transactions_k};
    ustore_database_describe_t describe {.outcome = &outcome, .description = &description};
    ustore_database_describe(&describe);
    failures += expect_failure(outcome);
    failures += expect_eq(static_cast<unsigned>(description.capabilities), 0u);

    ustore_snapshot_t snapshot = reinterpret_cast<ustore_snapshot_t>(stale_handle);
    ustore_stamp_t snapshot_stamp = stale_stamp;
    ustore_snapshot_init_t snapshot_init {.snapshot = &snapshot, .outcome = &outcome, .read_stamp = &snapshot_stamp};
    ustore_snapshot_init(&snapshot_init);
    failures += expect_failure(outcome);
    failures += expect(snapshot == nullptr && snapshot_stamp == 0);

    ustore_transaction_t transaction = nullptr;
    ustore_transaction_init_t transaction_init {.transaction = &transaction, .outcome = &outcome};
    ustore_transaction_init(&transaction_init);
    failures += expect_failure(outcome);
    failures += expect(transaction == nullptr);

    // A listing without an arena is refused before any engine runs
    ustore_size_t listed = 7;
    ustore_collection_list_t list {.database = database, .outcome = &outcome, .written = &listed};
    ustore_collection_list(&list);
    failures += expect_status(outcome, status_code_t::invalid_argument_k);
    failures += expect_eq(listed, 0u);

    // A stage and a commit of no transaction fail in the engine
    ustore_stamp_t read_stamp = stale_stamp;
    ustore_transaction_stage_t stage {.database = database, .outcome = &outcome, .read_stamp = &read_stamp};
    ustore_transaction_stage(&stage);
    failures += expect_failure(outcome);
    failures += expect_eq(read_stamp, 0u);
    ustore_stamp_t commit_stamp = stale_stamp;
    ustore_transaction_commit_t commit {.database = database, .outcome = &outcome, .commit_stamp = &commit_stamp};
    ustore_transaction_commit(&commit);
    failures += expect_failure(outcome);
    failures += expect_eq(commit_stamp, 0u);

    // A gist fails on a document whose path outgrows the longest one a gist exports
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    auto [documents, documents_status] = create_collection(database, "failed.documents", ustore_modality_docs_k);
    failures += expect(documents_status.succeeded());
    ustore_key_t const key = key_from_integer(450);
    std::string const document = fmt::format(R"({{"{}":1}})", std::string(600, 'a'));
    strided_strings<std::string> const document_strings {{&document, 0}};
    ustore_docs_write_t write {.database = database,
                               .outcome = &outcome,
                               .count = 1,
                               .collections = {&documents, 0},
                               .keys = {&key, 0},
                               .values = document_strings.view()};
    ustore_docs_write(&write);
    failures += expect_success(outcome);
    ustore_size_t paths = 7;
    ustore_strings_view_t fields {};
    ustore_docs_gist_t gist {.database = database,
                             .outcome = &outcome,
                             .count = 1,
                             .collections = {&documents, 0},
                             .keys = {&key, 0},
                             .fields = &fields,
                             .written = &paths,
                             .arena = arena};
    ustore_docs_gist(&gist);
    failures += expect_status(outcome, status_code_t::capacity_exhausted_k);
    failures += expect_eq(paths, 0u);

    failures += expect(create_collection(database, "failed.reports", ustore_modality_blobs_k).has_value());
    ustore_collection_id_t collection {uuid_from_integer(0x4242)};
    ustore_collection_create_t create {.database = database,
                                       .outcome = &outcome,
                                       .name = bytes_of("failed.reports"),
                                       .modality = ustore_modality_blobs_k,
                                       .collection = &collection};
    ustore_collection_create(&create);
    failures += expect_failure(outcome);
    failures += expect(collection == ustore_collection_id_t {});

    // An executor whose table outgrows its counters is never made
    ustore_executor_t executor = reinterpret_cast<ustore_executor_t>(stale_handle);
    ustore_executor_for_ram_t spawn {.executor = &executor,
                                     .outcome = &outcome,
                                     .threads = 1,
                                     .deferred_limit = ustore_size_t {1} << 31};
    ustore_executor_for_ram(&spawn);
    failures += expect_status(outcome, status_code_t::capacity_exhausted_k);
    failures += expect(executor == nullptr);
    return failures;
}

/** A transaction reset names the database the transaction belongs to, so a reset through another
 *  database is refused before any engine runs, and the transaction keeps serving its own. */
case_result_t database_transaction_reset_elsewhere(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_transactions()) return case_result_t::skipped("the engine keeps no transactions", failures);

    // The in-memory engine opens a second database without a directory, and a disk engine in a
    // directory of its own
    expected<database_t> other = status_t {status_code_t::uninitialized_k, "No second database"};
    if (config.engine() == "ram") other = open_database<database_t>(config_t {config.options(), nullptr});
    else if (config.directory()) {
        path_t const directory {config.directory(), "elsewhere"};
        failures += expect(recreate_directory(directory.c_str()));
        other = open_database<database_t>(config_t {config.options(), directory.c_str()});
    }
    else return case_result_t::skipped(needs_directory_k, failures);
    failures += expect(other.has_value());
    if (failures != 0) return failures;

    ustore_outcome_t outcome {};
    ustore_transaction_t transaction = nullptr;
    ustore_transaction_init_t begin {.transaction = &transaction, .database = database, .outcome = &outcome};
    ustore_transaction_init(&begin);
    failures += expect_success(outcome);
    ustore_transaction_t const begun = transaction;
    ustore_transaction_init_t reset_elsewhere {.transaction = &transaction, .database = *other, .outcome = &outcome};
    ustore_transaction_init(&reset_elsewhere);
    failures += expect_status(outcome, status_code_t::invalid_argument_k);
    failures += expect(transaction == begun);

    auto [blobs, blobs_status] = create_collection(database, "database_transaction_reset_elsewhere.blobs",
                                                   ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    ustore_key_t const key = key_from_integer(460);
    failures += write_text(database, blobs, {&key, 1}, "value", transaction);
    ustore_transaction_commit_t commit {.database = database, .outcome = &outcome, .transaction = transaction};
    ustore_transaction_commit(&commit);
    failures += expect_success(outcome);
    ustore_transaction_free(transaction);
    return failures;
}

/** A small read answers a tape of 32-bit offsets, @c count + 1 of them, the first at zero and the
 *  last closing the final value, beside lengths where a missing key reads as empty. */
std::size_t database_read_offsets(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "database_read_offsets.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());

    std::array<ustore_key_t, 3> const keys {key_from_integer(41), key_from_integer(42), key_from_integer(43)};
    failures += write_text(database, blobs, {&keys[0], 1}, "a");
    failures += write_text(database, blobs, {&keys[2], 1}, "bcd");
    read_columns_t columns;
    ustore_length_t const* lengths = nullptr;
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read = read_of(database, blobs, keys, columns, arena, outcome);
    read.lengths = &lengths;
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(columns.presences && lengths && tape_written(columns.values));
    if (failures != 0) return failures;

    failures += expect_eq(columns.presences[0], 0b101);
    failures += expect(columns.values.layout == ustore_strings_u32_tape_k);
    std::uint32_t const* const offsets = columns.values.data.u32_tape.offsets;
    failures += expect_eq(offsets[0], 0u);
    failures += expect_eq(offsets[1] - offsets[0], 1u);
    failures += expect_eq(offsets[2] - offsets[1], 0u);
    failures += expect_eq(offsets[3] - offsets[2], 3u);
    failures += expect(lengths[0] == 1 && lengths[1] == 0 && lengths[2] == 3);
    failures += expect_eq(columns.value(2), "bcd");
    return failures;
}

/** A u32 tape, a u64 tape and an accessor holding the same values, one of them empty, write
 *  the same bytes. */
std::size_t database_write_layouts(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "database_write_layouts.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());

    std::array<std::string_view, 3> const texts {"alpha", "", "gamma"};
    std::string_view const joined = "alphagamma";
    std::array<std::uint32_t, 4> const narrow_offsets {0, 5, 5, 10};
    std::array<std::uint64_t, 4> const wide_offsets {0, 5, 5, 10};
    strided_strings<std::string_view> const accessed {strided(texts).begin()};
    std::array<ustore_strings_view_t, 3> const layouts {u32_tape(joined.data(), narrow_offsets.data()),
                                                        u64_tape(joined.data(), wide_offsets.data()), accessed.view()};
    for (std::size_t layout = 0; layout != layouts.size(); ++layout) {
        std::array<ustore_key_t, 3> keys {};
        for (std::size_t index = 0; index != keys.size(); ++index)
            keys[index] = key_from_integer(1500 + layout * keys.size() + index);
        ustore_outcome_t outcome {};
        ustore_blobs_write_t write {};
        write.database = database;
        write.outcome = &outcome;
        write.count = keys.size();
        write.collections = {&blobs, 0};
        write.keys = {keys.data(), sizeof(ustore_key_t)};
        write.values = layouts[layout];
        ustore_blobs_write(&write);
        failures += expect_success(outcome);

        read_columns_t columns;
        ustore_blobs_read_t read = read_of(database, blobs, keys, columns, arena, outcome);
        ustore_blobs_read(&read);
        failures += expect_success(outcome);
        failures += expect(columns.presences && tape_written(columns.values));
        if (!columns.presences || !tape_written(columns.values)) continue;
        failures += expect_eq(columns.presences[0], 0b111);
        for (std::size_t index = 0; index != texts.size(); ++index)
            failures += expect_eq(columns.value(index), texts[index]);
    }
    return failures;
}

/** The state of an accessor answering one value for every index, which records the largest block it
 *  was asked for. */
struct repeating_accessor_t {
    std::string_view value;
    std::size_t* largest_block = nullptr;
};

/** Writes the one value of @p handle, a @c repeating_accessor_t, for each of @p count indices. */
void get_repeated(void const* handle, ustore_size_t const*, ustore_size_t count, ustore_bytes_view_t* views) noexcept {
    repeating_accessor_t const& accessor = *static_cast<repeating_accessor_t const*>(handle);
    *accessor.largest_block = std::max<std::size_t>(*accessor.largest_block, count);
    for (ustore_size_t index = 0; index != count; ++index) views[index] = bytes_of(accessor.value);
}

/** An accessor answering one value for every index broadcasts it to every key, asked in blocks of
 *  at most 64. */
std::size_t database_write_broadcast(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "database_write_broadcast.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());

    constexpr std::size_t count = 150;
    std::array<ustore_key_t, count> keys {};
    for (std::size_t index = 0; index != count; ++index) keys[index] = key_from_integer(1600 + index);
    std::size_t largest_block = 0;
    repeating_accessor_t const repeating {"same", &largest_block};
    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {};
    write.database = database;
    write.outcome = &outcome;
    write.count = count;
    write.collections = {&blobs, 0};
    write.keys = {keys.data(), sizeof(ustore_key_t)};
    write.values.layout = ustore_strings_accessor_k;
    write.values.data.accessor = {&get_repeated, &repeating};
    ustore_blobs_write(&write);
    failures += expect_success(outcome);
    failures += expect(largest_block != 0 && largest_block <= 64);

    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, keys, columns, arena, outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(tape_written(columns.values));
    if (!tape_written(columns.values)) return failures;
    std::size_t differing = 0;
    for (std::size_t index = 0; index != count; ++index) differing += columns.value(index) != "same";
    failures += expect_eq(differing, 0u);
    return failures;
}

/** A strings view in a layout no member of its union describes is refused
 *  with @c ustore_status_invalid_argument_k. */
std::size_t database_unknown_layout(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "database_unknown_layout.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());

    ustore_key_t const key = key_from_integer(1700);
    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {};
    write.database = database;
    write.outcome = &outcome;
    write.count = 1;
    write.collections = {&blobs, 0};
    write.keys = {&key, 0};
    write.values = stored_values_k.view();
    write.values.layout = static_cast<ustore_strings_layout_t>(3);
    ustore_blobs_write(&write);
    failures += expect_status(outcome, status_code_t::invalid_argument_k);
    return failures;
}

/** A zeroed view of fields keeps its default, the whole document, on a write and on a read. */
std::size_t database_zeroed_fields(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "database_zeroed_fields.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());

    auto [docs, docs_status] = create_collection(database, "zeroed.docs", ustore_modality_docs_k);
    failures += expect(docs_status.succeeded());
    if (failures != 0) return failures;

    ustore_key_t const key = key_from_integer(1800);
    std::string_view const document = R"({"a":1})";
    strided_strings<std::string_view> const documents {{&document, 0}};
    ustore_outcome_t outcome {};
    ustore_docs_write_t write {
        .database = database,
        .outcome = &outcome,
        .count = 1,
        .collections = {&docs, 0},
        .keys = {&key, 0},
        .values = documents.view(),
    };
    ustore_docs_write(&write);
    failures += expect_success(outcome);

    read_columns_t columns;
    ustore_docs_read_t read {
        .database = database,
        .outcome = &outcome,
        .count = 1,
        .collections = {&docs, 0},
        .keys = {&key, 0},
        .presences = &columns.presences,
        .values = &columns.values,
        .arena = arena,
    };
    ustore_docs_read(&read);
    failures += expect_success(outcome);
    failures += expect(columns.presences && tape_written(columns.values));
    if (failures != 0) return failures;
    failures += expect_eq(columns.presences[0] & 1, 1);
    failures += expect_eq(columns.value(0), document);
    return failures;
}

/** The verbs of every modality refuse a collection holding another one, so a document's bytes and
 *  keys stay out of reach of the blobs verbs, documents are drawn by their own sample, and a blobs
 *  collection takes no documents. */
case_result_t database_modality_refusals(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "modality.blobs", ustore_modality_blobs_k);
    auto [docs, docs_status] = create_collection(database, "modality.docs", ustore_modality_docs_k);
    auto [graph, graph_status] = create_collection(database, "modality.graph", ustore_modality_graph_k);
    failures += expect(blobs_status.succeeded() && docs_status.succeeded() && graph_status.succeeded());
    if (failures != 0) return failures;

    ustore_key_t const key = key_from_integer(1900);
    std::string_view const document = R"({"a":1})";
    strided_strings<std::string_view> const documents {{&document, 0}};
    ustore_outcome_t outcome {};
    ustore_docs_write_t write_document {
        .database = database,
        .outcome = &outcome,
        .count = 1,
        .collections = {&docs, 0},
        .keys = {&key, 0},
        .values = documents.view(),
    };
    ustore_docs_write(&write_document);
    failures += expect_success(outcome);

    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, {&key, 1}, columns, arena, outcome);
    read.collections = {&docs, 0};
    ustore_blobs_read(&read);
    failures += expect_status(status_t {outcome}, status_code_t::invalid_argument_k);

    // Documents draw their keys through a sample of their own, which the blobs sample never
    // stands in for
    ustore_length_t const* offsets = nullptr;
    ustore_key_t const* drawn = nullptr;
    ustore_docs_sample_t sample_documents {
        .database = database,
        .outcome = &outcome,
        .count = 1,
        .collections = {&docs, 0},
        .offsets = &offsets,
        .keys = &drawn,
        .arena = arena,
        .wanted = 4,
    };
    ustore_docs_sample(&sample_documents);
    failures += expect_success(outcome);
    failures += expect(offsets && drawn && offsets[1] == 1 && drawn[0] == key);
    sample_documents.collections = {&blobs, 0};
    ustore_docs_sample(&sample_documents);
    failures += expect_status(status_t {outcome}, status_code_t::invalid_argument_k);
    ustore_blobs_sample_t sample_blobs {
        .database = database,
        .outcome = &outcome,
        .count = 1,
        .collections = {&docs, 0},
        .offsets = &offsets,
        .keys = &drawn,
        .arena = arena,
        .wanted = 4,
    };
    ustore_blobs_sample(&sample_blobs);
    failures += expect_status(status_t {outcome}, status_code_t::invalid_argument_k);

    write_document.collections = {&blobs, 0};
    ustore_docs_write(&write_document);
    failures += expect_status(status_t {outcome}, status_code_t::invalid_argument_k);

    ustore_graph_write_edges_t write_edge {
        .database = database,
        .outcome = &outcome,
        .count = 1,
        .collections = {&docs, 0},
        .sources = {&key, 0},
        .targets = {&key, 0},
    };
    ustore_graph_write_edges(&write_edge);
    failures += expect_status(status_t {outcome}, status_code_t::invalid_argument_k);

    ustore_pages_import_t import {};
    import.database = database, import.outcome = &outcome, import.count = 1, import.collections = {&graph, 0};
    import.keys = {&key, 0}, import.values = documents.view(), import.encoding = ustore_page_encoding_xml_k;
    ustore_pages_import(&import);
    failures += expect_status(status_t {outcome}, status_code_t::invalid_argument_k);
    return failures;
}

/** The documents, the vertices and the pages each list their own keys, a vertex without
 *  edges included, and a document removal takes its key away, while no modality lists the
 *  keys of another. */
case_result_t database_modality_keys(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    auto [docs, docs_status] = create_collection(database, "keys.docs", ustore_modality_docs_k);
    auto [graph, graph_status] = create_collection(database, "keys.graph", ustore_modality_graph_k);
    auto [pages, pages_status] = create_collection(database, "keys.pages", ustore_modality_pages_k);
    failures += expect(docs_status.succeeded() && graph_status.succeeded() && pages_status.succeeded());
    if (failures != 0) return failures;

    std::array<ustore_key_t, 2> const keys {key_from_integer(1950), key_from_integer(1951)};
    std::string_view const text = R"({"a":1})";
    strided_strings<std::string_view> const repeated {{&text, 0}};
    ustore_outcome_t outcome {};
    ustore_docs_write_t write_documents {.database = database,
                                         .outcome = &outcome,
                                         .count = keys.size(),
                                         .collections = {&docs, 0},
                                         .keys = {keys.data(), sizeof(ustore_key_t)},
                                         .values = repeated.view()};
    ustore_docs_write(&write_documents);
    failures += expect_success(outcome);
    ustore_graph_write_vertices_t write_vertices {.database = database,
                                                  .outcome = &outcome,
                                                  .count = keys.size(),
                                                  .collections = {&graph, 0},
                                                  .vertices = {keys.data(), sizeof(ustore_key_t)}};
    ustore_graph_write_vertices(&write_vertices);
    failures += expect_success(outcome);
    std::string_view const markup = "<a/>";
    strided_strings<std::string_view> const markups {{&markup, 0}};
    ustore_pages_import_t import {};
    import.database = database, import.outcome = &outcome, import.count = keys.size(), import.collections = {&pages, 0};
    import.keys = {keys.data(), sizeof(ustore_key_t)}, import.values = markups.view();
    import.encoding = ustore_page_encoding_xml_k;
    ustore_pages_import(&import);
    failures += expect_success(outcome);

    // Lists the keys of one collection through @p scan, comparing them to @p wanted or the status
    // to @p refused
    auto expect_listed = [&]<typename scan_type_>(void (*scan)(scan_type_*), ustore_collection_id_t collection,
                                                  std::span<ustore_key_t const> wanted,
                                                  status_code_t refused = status_code_t::success_k) {
        ustore_length_t const* offsets = nullptr;
        ustore_key_t const* found = nullptr;
        scan_type_ arguments {};
        arguments.database = database, arguments.outcome = &outcome, arguments.count = 1;
        arguments.collections = {&collection, 0}, arguments.offsets = &offsets, arguments.keys = &found;
        arguments.arena = arena, arguments.wanted = 8;
        scan(&arguments);
        if (refused != status_code_t::success_k) return expect_status(status_t {outcome}, refused);
        std::size_t listing_failures = expect_success(outcome) + expect(offsets != nullptr);
        if (listing_failures) return listing_failures;
        std::span<ustore_key_t const> const listed {found + offsets[0], found + offsets[1]};
        return expect(std::equal(listed.begin(), listed.end(), wanted.begin(), wanted.end()));
    };
    failures += expect_listed(&ustore_docs_scan, docs, keys);
    failures += expect_listed(&ustore_graph_scan_vertices, graph, keys);
    failures += expect_listed(&ustore_pages_scan, pages, keys);
    failures += expect_listed(&ustore_blobs_scan, docs, {}, status_code_t::invalid_argument_k);
    failures += expect_listed(&ustore_docs_scan, graph, {}, status_code_t::invalid_argument_k);
    failures += expect_listed(&ustore_graph_scan_vertices, pages, {}, status_code_t::invalid_argument_k);
    failures += expect_listed(&ustore_pages_scan, docs, {}, status_code_t::invalid_argument_k);

    ustore_docs_remove_t remove {.database = database,
                                 .outcome = &outcome,
                                 .count = 1,
                                 .collections = {&docs, 0},
                                 .keys = {keys.data(), sizeof(ustore_key_t)}};
    ustore_docs_remove(&remove);
    failures += expect_success(outcome);
    failures += expect_listed(&ustore_docs_scan, docs, std::span {keys}.subspan(1));
    remove.collections = {&graph, 0};
    ustore_docs_remove(&remove);
    failures += expect_status(status_t {outcome}, status_code_t::invalid_argument_k);
    return failures;
}

/** A strings write takes its strings as a u64 tape and its keys as a strided view, and a read
 *  naming the same strings by a u32 tape finds every key. */
case_result_t database_strings_layouts(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    auto [strings, strings_status] = create_collection(database, "layouts.strings", ustore_modality_strings_k);
    failures += expect(strings_status.succeeded());
    if (failures != 0) return failures;

    std::string_view const joined = "applebanana";
    std::array<std::uint64_t, 3> const wide_offsets {0, 5, 11};
    std::array<std::uint32_t, 3> const narrow_offsets {0, 5, 11};
    std::array<ustore_key_t, 2> const keys {key_from_integer(1), key_from_integer(2)};
    ustore_outcome_t outcome {};
    ustore_strings_write_t write {
        .database = database,
        .outcome = &outcome,
        .count = keys.size(),
        .collections = {&strings, 0},
        .strings = u64_tape(joined.data(), wide_offsets.data()),
        .values = {keys.data(), sizeof(ustore_key_t)},
    };
    ustore_strings_write(&write);
    failures += expect_success(outcome);

    ustore_octet_t const* presences = nullptr;
    ustore_key_t const* values = nullptr;
    ustore_strings_read_t read {
        .database = database,
        .outcome = &outcome,
        .count = keys.size(),
        .collections = {&strings, 0},
        .strings = u32_tape(joined.data(), narrow_offsets.data()),
        .presences = &presences,
        .values = &values,
        .arena = arena,
    };
    ustore_strings_read(&read);
    failures += expect_success(outcome);
    failures += expect(presences && values);
    if (failures != 0) return failures;
    failures += expect_eq(presences[0], 0b11);
    failures += expect_eq(values[0], keys[0]);
    failures += expect_eq(values[1], keys[1]);
    return failures;
}

/** A measurement without start or end keys spans the whole collection on every engine. */
std::size_t database_measure_whole(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "database_measure_whole.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());

    ustore_key_t const key = key_from_integer(950);
    failures += write_text(database, blobs, {&key, 1}, "value");
    ustore_size_t const* min_cardinalities = nullptr;
    ustore_size_t const* max_cardinalities = nullptr;
    ustore_outcome_t outcome {};
    ustore_blobs_measure_t measure {
        .database = database,
        .outcome = &outcome,
        .count = 1,
        .collections = {&blobs, 0},
        .min_cardinalities = &min_cardinalities,
        .max_cardinalities = &max_cardinalities,
        .arena = arena,
    };
    ustore_blobs_measure(&measure);
    failures += expect_success(outcome);
    failures += expect(min_cardinalities && max_cardinalities);
    if (min_cardinalities && max_cardinalities) failures += expect(min_cardinalities[0] <= max_cardinalities[0]);
    return failures;
}

/** A removal takes away the keys it names and leaves the ones a write beside it stored. */
std::size_t database_remove(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "database_remove.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());

    std::array<ustore_key_t, 3> const keys {key_from_integer(51), key_from_integer(52), key_from_integer(53)};
    failures += write_text(database, blobs, keys, "value");

    // The first and the last key go in one batch, which strides over the middle one
    ustore_outcome_t outcome {};
    ustore_blobs_remove_t remove {};
    remove.database = database;
    remove.outcome = &outcome;
    remove.count = 2;
    remove.collections = {&blobs, 0};
    remove.keys = {keys.data(), 2 * sizeof(ustore_key_t)};
    ustore_blobs_remove(&remove);
    failures += expect_success(outcome);

    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, keys, columns, arena, outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(columns.presences && columns.presences[0] == 0b010);
    return failures;
}

/** A rollback withdraws a stage but keeps the writes, so the next commit publishes them. */
case_result_t database_rollback(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "database_rollback.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    if (!database.supports_transactions()) return case_result_t::skipped("the engine keeps no transactions", failures);
    expected<transaction_t> maybe_transaction = database.transaction();
    failures += expect(maybe_transaction.has_value());
    if (failures != 0) return failures;
    ustore_transaction_t const transaction = maybe_transaction->transaction();

    ustore_key_t const key = key_from_integer(61);
    failures += write_text(database, blobs, {&key, 1}, "staged", transaction);
    ustore_outcome_t outcome {};
    ustore_transaction_stage_t stage {.database = database, .outcome = &outcome, .transaction = transaction};
    ustore_transaction_stage(&stage);
    failures += expect_success(outcome);
    ustore_transaction_rollback_t rollback {.database = database, .outcome = &outcome, .transaction = transaction};
    ustore_transaction_rollback(&rollback);
    failures += expect_success(outcome);
    ustore_transaction_commit_t commit {.database = database, .outcome = &outcome, .transaction = transaction};
    ustore_transaction_commit(&commit);
    failures += expect_success(outcome);

    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, {&key, 1}, columns, arena, outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(tape_written(columns.values) && columns.value(0) == "staged");
    return failures;
}

/** A stage reads at a stamp no later than the commit after it, and a snapshot taken next reads at
 *  that commit or past it. */
case_result_t database_stamps(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "database_stamps.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    if (!database.supports_transactions() || !database.supports_snapshots())
        return case_result_t::skipped("the engine keeps no transactions or no snapshots", failures);

    // A commit at the head first, so the stage reads above zero and a commit reporting nothing
    // reads below it
    ustore_key_t const key = key_from_integer(62);
    failures += write_text(database, blobs, {&key, 1}, "head");
    expected<transaction_t> maybe_transaction = database.transaction();
    failures += expect(maybe_transaction.has_value());
    if (failures != 0) return failures;
    ustore_transaction_t const transaction = maybe_transaction->transaction();
    failures += write_text(database, blobs, {&key, 1}, "staged", transaction);

    ustore_stamp_t read_stamp = std::numeric_limits<ustore_stamp_t>::max();
    ustore_stamp_t commit_stamp = 0;
    ustore_stamp_t snapshot_stamp = 0;
    ustore_outcome_t outcome {};
    ustore_transaction_stage_t stage {
        .database = database,
        .outcome = &outcome,
        .transaction = transaction,
        .read_stamp = &read_stamp,
    };
    ustore_transaction_stage(&stage);
    failures += expect_success(outcome);
    ustore_transaction_commit_t commit {
        .database = database,
        .outcome = &outcome,
        .transaction = transaction,
        .commit_stamp = &commit_stamp,
    };
    ustore_transaction_commit(&commit);
    failures += expect_success(outcome);
    failures += expect(read_stamp <= commit_stamp);

    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t snapshot_init {
        .snapshot = &snapshot,
        .database = database,
        .outcome = &outcome,
        .read_stamp = &snapshot_stamp,
    };
    ustore_snapshot_init(&snapshot_init);
    failures += expect_success(outcome);
    failures += expect(commit_stamp <= snapshot_stamp);
    ustore_snapshot_free(snapshot);
    return failures;
}

/** Freeing a database before its transaction and snapshot leaves both working, and freeing the last
 *  of them closes the database, so it opens again. */
case_result_t database_free_holds(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_raw(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;

    ustore_outcome_t outcome {};
    ustore_database_description_t description {};
    ustore_database_describe_t describe {.database = database, .outcome = &outcome, .description = &description};
    ustore_database_describe(&describe);
    failures += expect_success(outcome);
    constexpr int needed_k = ustore_capabilities_transactions_k | ustore_capabilities_snapshots_k;
    if ((description.capabilities & needed_k) != needed_k) {
        ustore_database_free(database);
        return case_result_t::skipped("the engine keeps no transactions or no snapshots", failures);
    }
    auto [blobs, blobs_status] = create_collection(database, "database_free_holds.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());

    ustore_transaction_t transaction = nullptr;
    ustore_transaction_init_t transaction_init {.transaction = &transaction, .database = database, .outcome = &outcome};
    ustore_transaction_init(&transaction_init);
    failures += expect_success(outcome);
    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t snapshot_init {.snapshot = &snapshot, .database = database, .outcome = &outcome};
    ustore_snapshot_init(&snapshot_init);
    failures += expect_success(outcome);
    ustore_database_free(database);

    ustore_key_t const key = key_from_integer(90);
    failures += write_text(database, blobs, {&key, 1}, "held", transaction);
    ustore_transaction_commit_t commit {.database = database, .outcome = &outcome, .transaction = transaction};
    ustore_transaction_commit(&commit);
    failures += expect_success(outcome);

    // The snapshot predates the commit, so it still misses the key
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, {&key, 1}, columns, arena, outcome);
    read.view = ustore_snapshot_view(snapshot);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(columns.presences && (columns.presences[0] & 1) == 0);

    ustore_transaction_free(transaction);
    ustore_snapshot_free(snapshot);
    auto [reopened, reopen_status] = open_raw(config);
    failures += expect(reopen_status.succeeded());
    ustore_database_free(reopened);
    return failures;
}

/** A fresh database holds no collection until one is created, never listing its system collections,
 *  and refuses a name starting with an underscore, which they keep. */
case_result_t database_fresh_collections(config_t const& config) {
    std::size_t failures = 0;
    expected<database_t> fresh = status_t {status_code_t::uninitialized_k, "No fresh database"};
    if (config.engine() == "ram") fresh = open_database<database_t>(config_t {config.options(), nullptr});
    else if (config.directory()) {
        path_t const directory {config.directory(), "fresh"};
        failures += expect(recreate_directory(directory.c_str()));
        fresh = open_database<database_t>(config_t {config.options(), directory.c_str()});
    }
    else return case_result_t::skipped(needs_directory_k, failures);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(fresh.has_value() && arena_status.succeeded());
    if (failures != 0) return failures;

    auto [listed, list_status] = fresh->collections(arena);
    failures += expect(list_status.succeeded());
    failures += expect_eq(listed.size(), 0u);
    failures += expect_status(collection_named(fresh->receiver(), "_names"), status_code_t::not_found_k);
    failures += expect_status(create_collection(*fresh, "_x", ustore_modality_blobs_k),
                              status_code_t::invalid_argument_k);
    return failures;
}

/** A name resolves at the snapshot it is read at, so a collection created after the snapshot
 *  stays missing there. */
case_result_t database_find_at_snapshot(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_snapshots()) return case_result_t::skipped("the engine keeps no snapshots", failures);

    expected<snapshot_t> snapshot = database.snapshot();
    failures += expect(snapshot.has_value());
    if (failures != 0) return failures;
    failures += expect(create_collection(database, "later", ustore_modality_blobs_k).has_value());
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());

    std::string_view const name = "later";
    strided_strings<std::string_view> const names {{&name, 0}};
    ustore_octet_t const* presences = nullptr;
    ustore_collection_id_t const* found = nullptr;
    ustore_outcome_t outcome {};
    ustore_collection_find_t find {};
    find.database = database;
    find.outcome = &outcome;
    find.count = 1;
    find.names = names.view();
    find.presences = &presences;
    find.collections = &found;
    find.arena = arena;
    ustore_collection_find(&find);
    failures += expect_success(outcome);
    failures += expect(presences && found && presences[0] == 1);

    find.snapshot = snapshot->snapshot();
    ustore_collection_find(&find);
    failures += expect_success(outcome);
    failures += expect(presences && found && presences[0] == 0);
    if (found) failures += expect(found[0] == ustore_collection_id_t {});
    return failures;
}

/** Every status spelling reads back as its status, while a value or a name no status carries
 *  maps onto none. */
std::size_t status_names(config_t const&) {
    std::size_t failures = 0;
    std::size_t spelled = 0;
    // Values no status carries reach the library as a C caller passes them, never loaded as
    // the enum here
    for (int value = 0; value != 64; ++value) {
        std::string_view const name = ustore_status_name(static_cast<ustore_status_t>(value));
        if (name == "unrecognized") continue;
        ++spelled;
        failures += expect(ustore_status_from_name(bytes_of(name)) == value);
    }
    failures += expect_eq(spelled, std::size_t {19});
    failures += expect(std::string_view {ustore_status_name(ustore_status_deferred_k)} == "deferred");
    failures += expect(std::string_view {ustore_status_name(ustore_status_read_only_k)} == "read-only");
    failures += expect(ustore_status_from_name(bytes_of(std::string_view {"unrecognized"})) ==
                       ustore_status_invalid_argument_k);
    return failures;
}

/** Fails unless exactly @p count values below 64 have a spelling, each reading back as its value,
 *  while a name no enumerator carries reads back as the zero enumerator. */
template <typename enum_type_>
std::size_t expect_spellings(char const* (*name_of)(enum_type_), enum_type_ (*from_name)(ustore_bytes_view_t),
                             std::size_t count) {
    std::size_t failures = 0;
    std::size_t spelled = 0;
    // Values no enumerator carries reach the library as a C caller passes them, never loaded as
    // the enum here
    for (int value = 0; value != 64; ++value) {
        std::string_view const name = name_of(static_cast<enum_type_>(value));
        if (name == "unrecognized") continue;
        ++spelled;
        failures += expect(static_cast<int>(from_name(bytes_of(name))) == value);
    }
    failures += expect_eq(spelled, count);
    failures += expect(static_cast<int>(from_name(bytes_of(std::string_view {"unrecognized"}))) == 0);
    return failures;
}

/** Every closed set spells each of its enumerators and reads every spelling back. */
std::size_t closed_set_names(config_t const&) {
    std::size_t failures = 0;
    failures += expect_spellings(ustore_mood_name, ustore_mood_from_name, 2);
    failures += expect_spellings(ustore_durability_name, ustore_durability_from_name, 2);
    failures += expect_spellings(ustore_watch_name, ustore_watch_from_name, 2);
    failures += expect_spellings(ustore_scan_mode_name, ustore_scan_mode_from_name, 2);
    failures += expect_spellings(ustore_drop_mode_name, ustore_drop_mode_from_name, 3);
    failures += expect_spellings(ustore_isolation_name, ustore_isolation_from_name, 2);
    failures += expect_spellings(ustore_transport_name, ustore_transport_from_name, 5);
    failures += expect_spellings(ustore_gds_name, ustore_gds_from_name, 3);
    failures += expect_spellings(ustore_pattern_kind_name, ustore_pattern_kind_from_name, 2);
    failures += expect_spellings(ustore_case_sensitivity_name, ustore_case_sensitivity_from_name, 2);
    failures += expect_spellings(ustore_predicate_cost_name, ustore_predicate_cost_from_name, 2);
    failures += expect_spellings(ustore_engine_name, ustore_engine_from_name, 4);
    failures += expect_spellings(ustore_modality_name, ustore_modality_from_name, 5);
    failures += expect_spellings(ustore_vertex_role_name, ustore_vertex_role_from_name, 4);
    failures += expect_spellings(ustore_docs_field_type_name, ustore_docs_field_type_from_name, 17);
    failures += expect_spellings(ustore_docs_modification_name, ustore_docs_modification_from_name, 5);
    failures += expect_spellings(ustore_page_encoding_name, ustore_page_encoding_from_name, 5);
    failures += expect(std::string_view {ustore_isolation_name(ustore_isolation_strict_serializable_k)} ==
                       "strict-serializable");
    return failures;
}

/** A description names the engine that opened the database, and the RAM engine its isolation and
 *  its keys, while a compaction keeps every key. */
std::size_t database_describe_compact(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_raw(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "database_describe_compact.blobs",
                                                   ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    std::array<ustore_key_t, 3> const keys {key_from_integer(31), key_from_integer(32), key_from_integer(33)};
    failures += write_text(database, blobs, keys, "kept");

    ustore_outcome_t outcome {};
    ustore_database_description_t description {};
    ustore_database_describe_t describe {.database = database, .outcome = &outcome, .description = &description};
    ustore_database_describe(&describe);
    failures += expect_success(outcome);
    failures += expect(std::string_view {ustore_engine_name(description.engine)} == config.engine());
    if (config.engine() == "ram") {
        failures += expect(description.isolation ==
                           static_cast<ustore_isolation_t>(config.options().isolation.selected));
        failures += expect(description.keys >= keys.size());
    }

    ustore_database_compact_t compact {.database = database, .outcome = &outcome};
    ustore_database_compact(&compact);
    failures += expect_success(outcome);
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, keys, columns, arena, outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(columns.presences && columns.presences[0] == 7 && columns.value(2) == "kept");
    ustore_database_free(database);
    return failures;
}

/** A clear empties every value keeping the keys, then removes every pair keeping the collections,
 *  then drops every collection, as each mode asks. */
std::size_t database_clear_modes(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_raw(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "clear.kept", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    ustore_key_t const key = key_from_integer(77);
    failures += write_text(database, blobs, {&key, 1}, "value");

    ustore_outcome_t outcome {};
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, {&key, 1}, columns, arena, outcome);
    ustore_database_clear_t clear {.database = database, .outcome = &outcome, .mode = ustore_drop_values_k};
    ustore_database_clear(&clear);
    failures += expect_success(outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(columns.presences && columns.presences[0] == 1 && columns.value(0).empty());

    clear.mode = ustore_drop_pairs_k;
    ustore_database_clear(&clear);
    failures += expect_success(outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(columns.presences && columns.presences[0] == 0);
    failures += expect(collection_named(receiver_t {database}, "clear.kept").has_value());

    // A snapshot belongs to its holder, so the RAM and RocksDB engines drop no collection under it
    auto [pinned, pinned_status] = create_collection(database, "clear.pinned", ustore_modality_blobs_k);
    failures += expect(pinned_status.succeeded());
    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t snapshot_init {.snapshot = &snapshot, .database = database, .outcome = &outcome};
    ustore_snapshot_init(&snapshot_init);
    if (status_t {outcome}.succeeded()) {
        ustore_collection_drop_t drop {.database = database,
                                       .outcome = &outcome,
                                       .collection = pinned,
                                       .mode = ustore_drop_collection_k};
        ustore_collection_drop(&drop);
        bool const refuses = config.engine() == "ram" || config.engine() == "rocksdb";
        failures += refuses ? expect_status(outcome, status_code_t::invalid_argument_k) : expect_success(outcome);
        ustore_snapshot_free(snapshot);
    }

    clear.mode = ustore_drop_collection_k;
    ustore_database_clear(&clear);
    failures += expect_success(outcome);
    failures += expect_status(collection_named(receiver_t {database}, "clear.kept"), status_code_t::not_found_k);
    ustore_database_free(database);
    return failures;
}

/** A database RocksDB opens read-only serves reads, snapshots and their saves, and refuses every
 *  kind of call that would change it with @c ustore_status_read_only_k. */
case_result_t database_view_refusals(config_t const& config) {
    if (config.engine() != "rocksdb") return case_result_t::skipped("only RocksDB opens a database read-only");
    if (!config.directory()) return case_result_t::skipped("a view serves an existing directory");
    std::size_t failures = 0;
    path_t const directory {config.directory(), "viewed"};
    path_t const saved {config.directory(), "viewed.saved"};
    failures += expect(recreate_directory(directory.c_str()) && recreate_directory(saved.c_str()));
    ustore_bytes_view_t const path = bytes_of(std::string_view {directory.c_str()});
    ustore_key_t const key = key_from_integer(5);
    ustore_outcome_t outcome {};
    ustore_database_t database = nullptr;
    ustore_database_open_rocksdb_t open {.database = &database, .outcome = &outcome, .directory = path};
    ustore_database_open_rocksdb(&open);
    failures += expect_success(outcome);
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "view.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    failures += write_text(database, blobs, {&key, 1}, "viewed");
    auto [created, create_status] = create_collection(database, "view.kept", ustore_modality_blobs_k);
    failures += expect(create_status.succeeded());
    ustore_database_free(database);

    ustore_database_view_rocksdb_t view {.database = &database, .outcome = &outcome, .directory = path};
    ustore_database_view_rocksdb(&view);
    failures += expect_success(outcome);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, {&key, 1}, columns, arena, outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(tape_written(columns.values) && columns.value(0) == "viewed");
    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t snapshot_init {.snapshot = &snapshot, .database = database, .outcome = &outcome};
    ustore_snapshot_init(&snapshot_init);
    failures += expect_success(outcome);
    ustore_snapshot_save_t save {.snapshot = snapshot,
                                 .outcome = &outcome,
                                 .path = bytes_of(std::string_view {saved.c_str()})};
    ustore_snapshot_save(&save);
    failures += expect_success(outcome);
    ustore_snapshot_free(snapshot);

    ustore_blobs_write_t write {.database = database,
                                .outcome = &outcome,
                                .count = 1,
                                .collections = {&blobs, 0},
                                .keys = {&key, 0},
                                .values = stored_values_k.view()};
    ustore_blobs_write(&write);
    failures += expect_status(status_t {outcome}, status_code_t::read_only_k);
    ustore_blobs_remove_t remove {.database = database,
                                  .outcome = &outcome,
                                  .count = 1,
                                  .collections = {&blobs, 0},
                                  .keys = {&key, 0}};
    ustore_blobs_remove(&remove);
    failures += expect_status(status_t {outcome}, status_code_t::read_only_k);
    ustore_transaction_t transaction = nullptr;
    ustore_transaction_init_t begin {.transaction = &transaction, .database = database, .outcome = &outcome};
    ustore_transaction_init(&begin);
    failures += expect_status(status_t {outcome}, status_code_t::read_only_k);
    failures += expect(transaction == nullptr);
    failures += expect_status(create_collection(database, "view.created", ustore_modality_blobs_k),
                              status_code_t::read_only_k);
    ustore_collection_drop_t drop {.database = database, .outcome = &outcome, .collection = created};
    ustore_collection_drop(&drop);
    failures += expect_status(status_t {outcome}, status_code_t::read_only_k);
    ustore_database_clear_t clear {.database = database, .outcome = &outcome, .mode = ustore_drop_pairs_k};
    ustore_database_clear(&clear);
    failures += expect_status(status_t {outcome}, status_code_t::read_only_k);
    ustore_database_compact_t compact {.database = database, .outcome = &outcome};
    ustore_database_compact(&compact);
    failures += expect_status(status_t {outcome}, status_code_t::read_only_k);
    ustore_database_free(database);
    return failures;
}

#pragma region Progress

/** What a progress callback saw of the call it watches, kept in plain fields, so a sanitizer flags
 *  two callbacks of one call running at once. */
struct progress_record_t {

    /** The outcome of a deferred call, which must read deferred while its callback runs, or null
     *  for a blocking one. */
    ustore_outcome_t* outcome = nullptr;

    /** The report to cancel the call at, counting from one, or zero to let it run. */
    ustore_size_t cancelling_report = 0;

    ustore_size_t calls = 0;
    ustore_size_t finished = 0;
    ustore_size_t total = 0;
    ustore_size_t overlaps = 0;
    ustore_size_t decreases = 0;
    ustore_size_t after_final = 0;
    bool inside = false;
};

/** Records one report into the @c progress_record_t behind @p state, cancelling at its
 *  @c cancelling_report. */
ustore_continuation_t record_progress(ustore_size_t finished, ustore_size_t total, void* state) noexcept {
    progress_record_t& record = *static_cast<progress_record_t*>(state);
    record.overlaps += record.inside;
    record.inside = true;
    record.decreases += finished < record.finished;
    if (record.outcome) record.after_final += ustore_outcome_status(record.outcome) != ustore_status_deferred_k;
    record.finished = finished;
    record.total = total;
    ++record.calls;
    record.inside = false;
    return record.calls == record.cancelling_report ? ustore_cancel_k : ustore_continue_k;
}

/** A scan of three tasks whose callback cancels after the first finishes cancelled and leaves its
 *  outputs null, for keys and strings alike. */
case_result_t progress_cancel_scans(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "progress_cancel_scans.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    std::array<ustore_key_t, 3> const keys {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
    failures += write_text(database, blobs, keys, "scanned");

    ustore_outcome_t outcome {};
    progress_record_t record {.cancelling_report = 1};
    ustore_length_t const* offsets = nullptr;
    ustore_key_t const* found = nullptr;
    ustore_blobs_scan_t scan {.database = database,
                              .outcome = &outcome,
                              .count = 3,
                              .collections = {&blobs, 0},
                              .start_keys = {&keys[0], 0},
                              .offsets = &offsets,
                              .keys = &found,
                              .arena = arena,
                              .wanted = 2,
                              .progress = &record_progress,
                              .progress_state = &record};
    ustore_blobs_scan(&scan);
    failures += expect_status(status_t {outcome}, status_code_t::cancelled_k);
    failures += expect(offsets == nullptr && found == nullptr);
    failures += expect_eq(record.calls, 1u);
    failures += expect_eq(record.total, 3u);

    auto [strings, strings_status] = create_collection(database, "progress.strings", ustore_modality_strings_k);
    failures += expect(strings_status.succeeded());
    if (failures != 0) return failures;
    std::array<std::string_view, 2> const prefixes {"a", "b"};
    std::array<ustore_key_t, 2> const prefix_keys {key_from_integer(1), key_from_integer(2)};
    strided_strings<std::string_view> const accessed {strided(prefixes).begin()};
    ustore_strings_write_t write {.database = database,
                                  .outcome = &outcome,
                                  .count = prefixes.size(),
                                  .collections = {&strings, 0},
                                  .strings = accessed.view(),
                                  .values = {prefix_keys.data(), sizeof(ustore_key_t)}};
    ustore_strings_write(&write);
    failures += expect_success(outcome);
    record = {.cancelling_report = 1};
    ustore_strings_view_t texts {};
    ustore_strings_scan_t strings_scan {.database = database,
                                        .outcome = &outcome,
                                        .count = prefixes.size(),
                                        .collections = {&strings, 0},
                                        .prefixes = accessed.view(),
                                        .offsets = &offsets,
                                        .strings = &texts,
                                        .arena = arena,
                                        .wanted = 2,
                                        .progress = &record_progress,
                                        .progress_state = &record};
    ustore_strings_scan(&strings_scan);
    failures += expect_status(status_t {outcome}, status_code_t::cancelled_k);
    failures += expect(offsets == nullptr && !tape_written(texts));
    failures += expect_eq(record.calls, 1u);
    return failures;
}

/** A pull its callback cancels publishes nothing and leaves the cursor where it was, so the next
 *  pull repeats it. */
case_result_t progress_cancel_cursor(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    auto [collection, create_status] = create_collection(database, "progress.cursor", ustore_modality_blobs_k);
    failures += expect(create_status.succeeded());
    std::array<ustore_key_t, 3> const keys {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
    failures += write_text(database, collection, keys, "pulled");
    auto [cursor, cursor_status] = open_cursor(database, collection);
    failures += expect(cursor_status.succeeded());
    if (failures != 0) return failures;

    ustore_outcome_t outcome {};
    progress_record_t record {.cancelling_report = 1};
    pulled_keys_t pulled;
    ustore_blobs_cursor_next_t next = pull_of(database, cursor, pulled, arena, 2, outcome);
    next.progress = &record_progress;
    next.progress_state = &record;
    ustore_blobs_cursor_next(&next);
    failures += expect_status(status_t {outcome}, status_code_t::cancelled_k);
    failures += expect(pulled.keys == nullptr && pulled.written == 0 && !tape_written(pulled.values));
    failures += expect(record.calls == 1 && record.finished == 1 && record.total == 1);

    next.progress = nullptr;
    ustore_blobs_cursor_next(&next);
    failures += expect_success(outcome);
    failures += expect_page(pulled, {keys.data(), 2}, "pulled");
    ustore_blobs_cursor_free(cursor);
    return failures;
}

/** Whether the saved copy at @p path is gone: no image in the directory the RAM engine saves into,
 *  and no database where RocksDB and LevelDB create one. */
bool nothing_saved(config_t const& config, char const* path) noexcept {
    std::error_code error;
    if (config.engine() == "ram")
        return !std::filesystem::exists(path_t {path, "ram.image"}.c_str(), error) &&
               !std::filesystem::exists(path_t {path, "ram.image.partial"}.c_str(), error);
    return !std::filesystem::exists(path, error) || std::filesystem::is_empty(path, error);
}

/** Readies @p path for a save: a fresh directory the RAM engine saves into, or no path where the
 *  others create it. */
bool ready_save_path(config_t const& config, char const* path) noexcept {
    if (config.engine() == "ram") return recreate_directory(path);
    std::error_code error;
    std::filesystem::remove_all(path, error);
    return !error;
}

/** Opens the copy saved at @p path with the engine of @p config. */
expected<ustore_database_t> open_saved(config_t const& config, char const* path) noexcept {
    engine_options_t options = config.options();
    options.directory = path;
    ustore_database_t database = nullptr;
    ustore_outcome_t outcome {};
    open_engine(options, &database, &outcome);
    if (status_t const status {outcome}; status.failed()) return status;
    return database;
}

/** A RAM image that exists but does not open fails the open, rather than opening an empty
 *  database over it. */
case_result_t database_unreadable_image(config_t const& config) {
    if (config.engine() != "ram" || !config.directory())
        return case_result_t::skipped("only the RAM engine keeps an image in a directory");
    path_t const directory {config.directory(), "unreadable.image"};
    path_t const image {directory.c_str(), "ram.image"};
    std::size_t failures = expect(recreate_directory(directory.c_str()));
    // A link to itself answers ELOOP to every open, even a privileged one
    std::error_code error;
    std::filesystem::create_symlink(image.c_str(), image.c_str(), error);
    failures += expect(!error);
    failures += expect_status(open_saved(config, directory.c_str()), status_code_t::io_k);
    return failures;
}

/** A save its callback cancels at the first report removes what it wrote, so a retry at the
 *  same path succeeds. */
case_result_t progress_cancel_save(config_t const& config) {
    if (!config.directory()) return case_result_t::skipped("a save needs a directory");
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "progress_cancel_save.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    if (!database.supports_snapshots()) return case_result_t::skipped("the engine keeps no snapshots", failures);
    ustore_key_t const key = key_from_integer(9);
    failures += write_text(database, blobs, {&key, 1}, "saved");
    path_t const saved {config.directory(), "progress.saved"};
    failures += expect(ready_save_path(config, saved.c_str()));

    ustore_outcome_t outcome {};
    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t snapshot_init {.snapshot = &snapshot, .database = database, .outcome = &outcome};
    ustore_snapshot_init(&snapshot_init);
    failures += expect_success(outcome);
    progress_record_t record {.cancelling_report = 1};
    ustore_snapshot_save_t save {.snapshot = snapshot,
                                 .outcome = &outcome,
                                 .path = bytes_of(std::string_view {saved.c_str()}),
                                 .progress = &record_progress,
                                 .progress_state = &record};
    ustore_snapshot_save(&save);
    failures += expect_status(status_t {outcome}, status_code_t::cancelled_k);
    failures += expect(record.calls == 1 && record.total != 0);
    failures += expect(nothing_saved(config, saved.c_str()));

    save.progress = nullptr;
    // A RAM save failing past its open removes the partial image too, here as a directory holds
    // the image's name
    if (config.engine() == "ram") {
        path_t const blocker {saved.c_str(), "ram.image"};
        failures += expect(recreate_directory(path_t {blocker.c_str(), "kept"}.c_str()));
        ustore_snapshot_save(&save);
        failures += expect_status(status_t {outcome}, status_code_t::io_k);
        std::error_code error;
        failures += expect(!std::filesystem::exists(path_t {saved.c_str(), "ram.image.partial"}.c_str(), error));
        std::filesystem::remove_all(blocker.c_str(), error);
    }
    ustore_snapshot_save(&save);
    failures += expect_success(outcome);
    ustore_snapshot_free(snapshot);
    auto [copy, copy_status] = open_saved(config, saved.c_str());
    failures += expect(copy_status.succeeded());
    ustore_database_free(copy);
    return failures;
}

/** A compaction its callback cancels at the first report stops between collections, and the
 *  data stays readable. */
case_result_t progress_cancel_compact(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    auto [blobs, blobs_status] = create_collection(database, "progress.compacted", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    ustore_key_t const key = key_from_integer(11);
    failures += write_text(database, blobs, {&key, 1}, "compacted");

    ustore_outcome_t outcome {};
    progress_record_t record {.cancelling_report = 1};
    ustore_database_compact_t compact {.database = database,
                                       .outcome = &outcome,
                                       .progress = &record_progress,
                                       .progress_state = &record};
    ustore_database_compact(&compact);
    failures += expect_status(status_t {outcome}, status_code_t::cancelled_k);
    failures += expect_eq(record.calls, 1u);
    failures += expect(record.finished <= record.total);

    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, {&key, 1}, columns, arena, outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(tape_written(columns.values) && columns.value(0) == "compacted");
    return failures;
}

/** The callbacks of a deferred scan, pull, compaction and save each run one at a time, before their
 *  call is final, with counts that never decrease and end at the total. The record counts without
 *  atomics, so ThreadSanitizer flags two callbacks of one call running at once. */
std::size_t progress_serialized(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [executor, spawn_status] = spawn_executor(0);
    failures += expect(open_status.succeeded() && spawn_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());
    auto [blobs, blobs_status] = create_collection(database, "progress_serialized.blobs", ustore_modality_blobs_k);
    failures += expect(blobs_status.succeeded());
    std::array<ustore_key_t, 4> const keys {key_from_integer(1), key_from_integer(2), key_from_integer(3),
                                            key_from_integer(4)};
    failures += write_text(database, blobs, keys, "reported");
    if (failures != 0) return failures;

    // Every report must come back clean, and a wait between the calls checks none comes late
    auto const expect_clean = [&](progress_record_t const& record, ustore_size_t calls) {
        std::size_t clean = expect_eq(record.calls, calls);
        clean += expect(record.overlaps == 0 && record.decreases == 0 && record.after_final == 0);
        return clean + expect_eq(record.finished, record.total);
    };
    ustore_outcome_t outcome {};
    progress_record_t scanned {.outcome = &outcome};
    ustore_length_t const* offsets = nullptr;
    ustore_key_t const* found = nullptr;
    ustore_blobs_scan_t scan {.database = database,
                              .executor = executor,
                              .outcome = &outcome,
                              .completion = ustore_completion_deferred_k,
                              .count = 64,
                              .collections = {&blobs, 0},
                              .start_keys = {&keys[0], 0},
                              .offsets = &offsets,
                              .keys = &found,
                              .arena = arena,
                              .wanted = 2,
                              .progress = &record_progress,
                              .progress_state = &scanned};
    ustore_blobs_scan(&scan);
    failures += expect(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    failures += expect_clean(scanned, 64);

    progress_record_t pulled_record {.outcome = &outcome};
    auto [cursor, cursor_status] = open_cursor(database, blobs);
    failures += expect(cursor_status.succeeded());
    pulled_keys_t pulled;
    ustore_blobs_cursor_next_t next = pull_of(database, cursor, pulled, arena, 2, outcome);
    next.executor = executor;
    next.completion = ustore_completion_deferred_k;
    next.progress = &record_progress;
    next.progress_state = &pulled_record;
    ustore_blobs_cursor_next(&next);
    failures += expect(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    failures += expect_clean(pulled_record, 1);
    ustore_blobs_cursor_free(cursor);

    progress_record_t compacted {.outcome = &outcome};
    ustore_database_compact_t compact {.database = database,
                                       .executor = executor,
                                       .outcome = &outcome,
                                       .completion = ustore_completion_deferred_k,
                                       .progress = &record_progress,
                                       .progress_state = &compacted};
    ustore_database_compact(&compact);
    failures += expect(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    failures += expect(compacted.calls != 0);
    failures += expect_clean(compacted, compacted.calls);

    if (config.directory() && database.supports_snapshots()) {
        path_t const saved {config.directory(), "progress.serialized"};
        failures += expect(ready_save_path(config, saved.c_str()));
        ustore_snapshot_t snapshot = nullptr;
        ustore_snapshot_init_t snapshot_init {.snapshot = &snapshot, .database = database, .outcome = &outcome};
        ustore_snapshot_init(&snapshot_init);
        failures += expect_success(outcome);
        progress_record_t saved_record {.outcome = &outcome};
        ustore_snapshot_save_t save {.snapshot = snapshot,
                                     .executor = executor,
                                     .outcome = &outcome,
                                     .completion = ustore_completion_deferred_k,
                                     .path = bytes_of(std::string_view {saved.c_str()}),
                                     .progress = &record_progress,
                                     .progress_state = &saved_record};
        ustore_snapshot_save(&save);
        failures += expect(ustore_outcome_wait(&outcome) == ustore_status_success_k);
        failures += expect(saved_record.calls != 0 && saved_record.total != 0);
        failures += expect_clean(saved_record, saved_record.calls);
        ustore_snapshot_free(snapshot);
    }

    std::size_t const calls = scanned.calls + compacted.calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    failures += expect_eq(scanned.calls + compacted.calls, calls);
    ustore_executor_free(executor);
    return failures;
}

#pragma endregion Progress

/** A save copies exactly the collections and names its snapshot recorded: one created after the
 *  snapshot stays behind, and one dropped after it comes along with its pairs. */
case_result_t snapshot_save_recorded(config_t const& config) {
    if (!config.directory()) return case_result_t::skipped("a save needs a directory");
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_snapshots()) return case_result_t::skipped("the engine keeps no snapshots", failures);
    auto [dropped, dropped_status] = create_collection(database, "save.dropped", ustore_modality_blobs_k);
    failures += expect(dropped_status.succeeded());
    ustore_key_t const key = key_from_integer(21);
    failures += write_text(database, dropped, {&key, 1}, "recorded");
    if (failures != 0) return failures;

    ustore_outcome_t outcome {};
    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t snapshot_init {.snapshot = &snapshot, .database = database, .outcome = &outcome};
    ustore_snapshot_init(&snapshot_init);
    failures += expect_success(outcome);
    failures += expect(create_collection(database, "save.created", ustore_modality_blobs_k).has_value());
    ustore_collection_drop_t drop {.database = database, .outcome = &outcome, .collection = dropped};
    ustore_collection_drop(&drop);
    bool const refuses_drop = config.engine() == "ram" || config.engine() == "rocksdb";
    failures += refuses_drop ? expect_status(outcome, status_code_t::invalid_argument_k) : expect_success(outcome);

    path_t const saved {config.directory(), "save.recorded"};
    failures += expect(ready_save_path(config, saved.c_str()));
    ustore_snapshot_save_t save {.snapshot = snapshot,
                                 .outcome = &outcome,
                                 .path = bytes_of(std::string_view {saved.c_str()})};
    ustore_snapshot_save(&save);
    failures += expect_success(outcome);
    ustore_snapshot_free(snapshot);

    auto [copy, copy_status] = open_saved(config, saved.c_str());
    failures += expect(copy_status.succeeded());
    if (failures != 0) return failures;
    failures += expect_status(collection_named(receiver_t {copy}, "save.created"), status_code_t::not_found_k);
    auto [kept, kept_status] = collection_named(receiver_t {copy}, "save.dropped");
    failures += expect(kept_status.succeeded() && kept == dropped);
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(copy, dropped, {&key, 1}, columns, arena, outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(tape_written(columns.values) && columns.value(0) == "recorded");
    ustore_database_free(copy);
    return failures;
}

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

    report(totals, "status.names", status_names, config);
    report(totals, "names.closed_sets", closed_set_names, config);
    report(totals, "database.init_refusals", database_init_refusals, config);
    report(totals, "database.validation", database_validation, config);
    report(totals, "database.forged_views", database_forged_views, config);
    report(totals, "database.foreign_handles", database_foreign_handles, config);
    report(totals, "database.failed_reports_zeroed", database_failed_reports_zeroed, config);
    report(totals, "database.transaction_reset_elsewhere", database_transaction_reset_elsewhere, config);
    report(totals, "database.read_offsets", database_read_offsets, config);
    report(totals, "database.write_layouts", database_write_layouts, config);
    report(totals, "database.write_broadcast", database_write_broadcast, config);
    report(totals, "database.unknown_layout", database_unknown_layout, config);
    report(totals, "database.zeroed_fields", database_zeroed_fields, config);
    report(totals, "database.modality_refusals", database_modality_refusals, config);
    report(totals, "database.modality_keys", database_modality_keys, config);
    report(totals, "database.strings_layouts", database_strings_layouts, config);
    report(totals, "database.measure_whole", database_measure_whole, config);
    report(totals, "database.remove", database_remove, config);
    report(totals, "database.rollback", database_rollback, config);
    report(totals, "database.stamps", database_stamps, config);
    report(totals, "database.free_holds", database_free_holds, config);
    report(totals, "database.find_at_snapshot", database_find_at_snapshot, config);
    report(totals, "database.fresh_collections", database_fresh_collections, config);
    report(totals, "database.describe_compact", database_describe_compact, config);
    report(totals, "database.clear_modes", database_clear_modes, config);
    report(totals, "database.view_refusals", database_view_refusals, config);
    report(totals, "progress.cancel_scans", progress_cancel_scans, config);
    report(totals, "progress.cancel_cursor", progress_cancel_cursor, config);
    report(totals, "database.unreadable_image", database_unreadable_image, config);
    report(totals, "progress.cancel_save", progress_cancel_save, config);
    report(totals, "progress.cancel_compact", progress_cancel_compact, config);
    report(totals, "progress.serialized", progress_serialized, config);
    report(totals, "snapshot.save_recorded", snapshot_save_recorded, config);
    return suite_exit_code(totals);
}
