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
#include "ustore/utilities/blocking_files.hpp" // `read_file`

#include "abi/fixture.hpp" // `write_text`, `create_collection`, `read_of`, `open_cursor`, `pull_of`, `expect_page`
#include "harness.hpp"     // `finish_test_run`, `expect_true`, `expect_status`, `open_on_own_executor`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::test::abi;
using namespace unum::ustore::utilities;
using namespace unum;

/** The reason a case reports on an engine other than the in-memory one. */
constexpr char const* needs_ram_engine_k = "needs the ram engine";

/** The reason a case reports on an engine that keeps no transactions or no snapshots. */
constexpr char const* needs_transactions_and_snapshots_k = "needs transactions and snapshots";

/** Every engine refuses an init without an output, and all but the in-memory one refuse to open
 *  without a directory. */
void test_database_init_refusals(config_t const& config) {
    ustore_outcome_t outcome {};
    open_on_own_executor(config.options(), nullptr, &outcome);
    expect_status(outcome, status_code_t::invalid_argument_k);

    engine_options_t without_directory = config.options();
    without_directory.directory = nullptr;
    ustore_database_t database = nullptr;
    open_on_own_executor(without_directory, &database, &outcome);
    config.engine() == "ram" ? expect_success(outcome) : expect_status(outcome, status_code_t::invalid_argument_k);
    ustore_database_free(database);
}

/** Writes, reads, transactions and scans accept valid tasks and reject malformed ones,
 *  per engine capability. */
void test_database_validation(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    auto [blobs, blobs_status] = create_collection(database, "database_validation.blobs", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());
    expected<transaction_t> maybe_transaction = database.on().transaction();
    ustore_transaction_t const transaction = maybe_transaction ? maybe_transaction->transaction() : nullptr;
    auto [arena, arena_status] = arena_t::make();
    expect_true(arena_status.succeeded());

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
        expect_success(outcome);
    }

    // Collections nobody created, the nil UUID among them
    std::array<ustore_collection_id_t, count> collections {
        {{uuid_from_integer(1)}, {uuid_from_integer(2)}, {uuid_from_integer(3)}}};
    ustore_blobs_write_t write_unnamed = write_head;
    write_unnamed.collections = {collections.data(), sizeof(ustore_collection_id_t)};
    ustore_blobs_write(&write_unnamed);
    expect_status(outcome, status_code_t::not_found_k);

    ustore_collection_id_t const nil_collection {};
    ustore_blobs_write_t write_nil = write_head;
    write_nil.collections = {&nil_collection, 0};
    ustore_blobs_write(&write_nil);
    expect_status(outcome, status_code_t::not_found_k);

    // No collection at all
    ustore_blobs_write_t write_null_collection = write_head;
    write_null_collection.collections = {};
    ustore_blobs_write(&write_null_collection);
    expect_status(outcome, status_code_t::invalid_argument_k);

    if (transaction) {
        ustore_blobs_write_t write = write_head;
        write.transaction = transaction;
        ustore_blobs_write(&write);
        expect_success(outcome);

        // A transaction flushes at commit, not at a write
        write.durability = ustore_durability_flushed_k;
        ustore_blobs_write(&write);
        expect_status(outcome, status_code_t::invalid_argument_k);
    }

    // No tasks, yet keys are given
    ustore_blobs_write_t write_no_tasks = write_head;
    write_no_tasks.count = 0;
    ustore_blobs_write(&write_no_tasks);
    expect_status(outcome, status_code_t::invalid_argument_k);

    // Tasks without keys
    ustore_blobs_write_t write_null_keys = write_head;
    write_null_keys.keys = {};
    ustore_blobs_write(&write_null_keys);
    expect_status(outcome, status_code_t::invalid_argument_k);

    // Tasks without values, which no default stands for
    ustore_blobs_write_t write_null_values = write_head;
    write_null_values.values = {};
    ustore_blobs_write(&write_null_values);
    expect_status(outcome, status_code_t::invalid_argument_k);

    // Keys stay unwatched only inside a transaction
    ustore_blobs_write_t write_blind_head = write_head;
    write_blind_head.watch = ustore_watch_none_k;
    ustore_blobs_write(&write_blind_head);
    expect_status(outcome, status_code_t::invalid_argument_k);

    // A strings write checks its durability as every other write does
    auto [strings, strings_status] = create_collection(database, "database_validation.strings",
                                                       ustore_modality_strings_k);
    expect_true(strings_status.succeeded());
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
    expect_status(outcome, status_code_t::invalid_argument_k);

#pragma endregion Writes

#pragma region Reads

    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, keys, columns, arena, outcome);
    ustore_blobs_read(&read);
    expect_success(outcome);
    if (tape_written(columns.values)) expect_equal(columns.value(2).size(), sizeof(std::uint64_t));

    if (transaction) {
        read.view = ustore_transaction_view(transaction, ustore_watch_none_k);
        ustore_blobs_read(&read);
        expect_success(outcome);
    }

    // A gist names its documents by their keys
    auto [docs, docs_status] = create_collection(database, "database_validation.docs", ustore_modality_docs_k);
    expect_true(docs_status.succeeded());
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
    expect_status(outcome, status_code_t::invalid_argument_k);

#pragma endregion Reads

#pragma region Transactions

    ustore_transaction_t raw_transaction = nullptr;
    ustore_transaction_init_t transaction_init {
        .transaction = &raw_transaction,
        .database = database,
        .outcome = &outcome,
    };
    ustore_transaction_init(&transaction_init);
    expect_true((outcome.status == ustore_status_success_k) == database.on().supports_transactions());
    outcome = {};

    // An engine without transactions refuses every transaction call before it reads the arguments
    bool const transactional = database.on().supports_transactions();
    transaction_init.transaction = nullptr;
    ustore_transaction_init(&transaction_init);
    expect_status(outcome, transactional ? status_code_t::invalid_argument_k : status_code_t::unsupported_k);

    // A commit needs the transaction it publishes
    ustore_transaction_commit_t transaction_commit {
        .database = database,
        .outcome = &outcome,
    };
    ustore_transaction_commit(&transaction_commit);
    expect_status(outcome, transactional ? status_code_t::uninitialized_k : status_code_t::unsupported_k);
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
    expect_success(outcome);

    // Without start keys a scan starts at the first key
    ustore_blobs_scan_t scan_without_start_keys = scan;
    scan_without_start_keys.start_keys = {};
    ustore_blobs_scan(&scan_without_start_keys);
    expect_success(outcome);

    ustore_blobs_scan_t scan_without_keys_output = scan;
    scan_without_keys_output.keys = nullptr;
    ustore_blobs_scan(&scan_without_keys_output);
    expect_status(outcome, status_code_t::invalid_argument_k);

#pragma endregion Scans
}

/** A pointer neither view constructor made, a raw handle with a stray bit above the tag or a bare
 *  transaction, is refused before any engine reads through it. */
void test_database_forged_views(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    auto [arena, arena_status] = arena_t::make();
    expect_true(arena_status.succeeded());
    auto [blobs, blobs_status] = create_collection(database, "database_forged_views.blobs", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());

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
    expect_status(outcome, status_code_t::invalid_argument_k);

    expected<transaction_t> maybe_transaction = database.on().transaction();
    if (!maybe_transaction) return;
    read.view = reinterpret_cast<ustore_view_t>(maybe_transaction->transaction());
    ustore_blobs_read(&read);
    expect_status(outcome, status_code_t::invalid_argument_k);
}

/** A view or a transaction of another engine is refused before either engine sees it. */
case_result_t test_database_foreign_handles(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return {};

    // The in-memory engine needs no directory, so it is the other engine beside every disk engine
    database_t other;
    if (config.engine() != "ram") {
        engine_options_t ram = config.options();
        ram.name = "ram";
        auto [opened, other_status] = open_database<database_t>(config_t {ram, nullptr});
        expect_true(other_status.succeeded());
        other = std::move(opened);
    }
    else if (config.directory()) {
        std::string const directory = fmt::format("{}/{}", config.directory(), "other");
        expect_true(scratch_directory_t::recreate(directory.c_str()));
        for (std::string_view const name : std::array<std::string_view, 2> {"rocksdb", "leveldb"}) {
            engine_options_t disk = config.options();
            disk.name = name;
            if (auto opened = open_database<database_t>(config_t {disk, directory.c_str()}); opened) {
                other = *std::move(opened);
                break;
            }
        }
    }
    else return case_result_t::skipped(needs_directory_k);
    if (!other) return case_result_t::skipped("needs a second engine");

    expected<transaction_t> maybe_transaction = other.on().transaction();
    expected<snapshot_t> maybe_snapshot = other.on().snapshot();
    // A transaction is tried first, since it is refused both as a view and as the transaction
    // of a write
    ustore_view_t foreign = nullptr;
    if (maybe_transaction) foreign = ustore_transaction_view(maybe_transaction->transaction(), ustore_watch_keys_k);
    else if (maybe_snapshot) foreign = ustore_snapshot_view(maybe_snapshot->snapshot());
    else return case_result_t::skipped("needs views from the second engine");

    auto [arena, arena_status] = arena_t::make();
    expect_true(arena_status.succeeded());
    auto [blobs, blobs_status] = create_collection(database, "database_foreign_handles.blobs", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());
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
    expect_status(outcome, status_code_t::invalid_argument_k);
    if (!maybe_transaction) return {};

    ustore_blobs_write_t write {};
    write.database = database;
    write.outcome = &outcome;
    write.transaction = maybe_transaction->transaction();
    write.count = 1;
    write.collections = {&blobs, 0};
    write.keys = {&key, 0};
    write.values = stored_values_k.view();
    ustore_blobs_write(&write);
    expect_status(outcome, status_code_t::invalid_argument_k);
    return {};
}

/** A call that fails zeroes the scalars it reports and leaves the handles it makes @c NULL,
 *  whatever they held: a description, a snapshot and its stamp, a listing's count, a collection,
 *  the stamps of a stage and of a commit, a gist's count, a transaction and an executor. */
void test_database_failed_reports_zeroed(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    ustore_outcome_t outcome {};
    std::uintptr_t const stale_handle = 0x40;
    ustore_stamp_t const stale_stamp = 77;

    // Without a database every call is uninitialized, which only the router sees
    ustore_database_description_t description {.capabilities = ustore_capabilities_transactions_k};
    ustore_database_describe_t describe {.outcome = &outcome, .description = &description};
    ustore_database_describe(&describe);
    expect_status(outcome, status_code_t::uninitialized_k);
    expect_equal(static_cast<unsigned>(description.capabilities), 0u);

    ustore_snapshot_t snapshot = reinterpret_cast<ustore_snapshot_t>(stale_handle);
    ustore_stamp_t snapshot_stamp = stale_stamp;
    ustore_snapshot_init_t snapshot_init {.snapshot = &snapshot, .outcome = &outcome, .read_stamp = &snapshot_stamp};
    ustore_snapshot_init(&snapshot_init);
    expect_status(outcome, status_code_t::uninitialized_k);
    expect_true(snapshot == nullptr && snapshot_stamp == 0);

    ustore_transaction_t transaction = nullptr;
    ustore_transaction_init_t transaction_init {.transaction = &transaction, .outcome = &outcome};
    ustore_transaction_init(&transaction_init);
    expect_status(outcome, status_code_t::uninitialized_k);
    expect_true(transaction == nullptr);

    // A listing without an arena is refused before any engine runs
    ustore_size_t listed = 7;
    ustore_collection_list_t list {.database = database, .outcome = &outcome, .written = &listed};
    ustore_collection_list(&list);
    expect_status(outcome, status_code_t::invalid_argument_k);
    expect_equal(listed, 0u);

    // A stage and a commit of no transaction fail in the engine, which refuses both
    // without transactions
    status_code_t const no_transaction = database.on().supports_transactions() ? status_code_t::uninitialized_k
                                                                               : status_code_t::unsupported_k;
    ustore_stamp_t read_stamp = stale_stamp;
    ustore_transaction_stage_t stage {.database = database, .outcome = &outcome, .read_stamp = &read_stamp};
    ustore_transaction_stage(&stage);
    expect_status(outcome, no_transaction);
    expect_equal(read_stamp, 0u);
    ustore_stamp_t commit_stamp = stale_stamp;
    ustore_transaction_commit_t commit {.database = database, .outcome = &outcome, .commit_stamp = &commit_stamp};
    ustore_transaction_commit(&commit);
    expect_status(outcome, no_transaction);
    expect_equal(commit_stamp, 0u);

    // A gist fails on a document whose path outgrows the longest one a gist exports
    auto [arena, arena_status] = arena_t::make();
    expect_true(arena_status.succeeded());
    auto [documents, documents_status] = create_collection(database, "failed.documents", ustore_modality_docs_k);
    expect_true(documents_status.succeeded());
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
    expect_success(outcome);
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
    expect_status(outcome, status_code_t::capacity_exhausted_k);
    expect_equal(paths, 0u);

    expect_true(create_collection(database, "failed.reports", ustore_modality_blobs_k).has_value());
    ustore_collection_id_t collection {uuid_from_integer(0x4242)};
    ustore_collection_create_t create {.database = database,
                                       .outcome = &outcome,
                                       .name = bytes_of("failed.reports"),
                                       .modality = ustore_modality_blobs_k,
                                       .collection = &collection};
    ustore_collection_create(&create);
    expect_status(outcome, status_code_t::already_exists_k);
    expect_true(collection == ustore_collection_id_t {});

    // An executor capped at a value no CPU class takes, here amd-genoa's retired id, is never made
    ustore_executor_t executor = reinterpret_cast<ustore_executor_t>(stale_handle);
    ustore_executor_for_ram_t spawn {.executor = &executor,
                                     .outcome = &outcome,
                                     .threads = 1,
                                     .cpu_class_ceiling = static_cast<ustore_cpu_class_t>(7)};
    ustore_executor_for_ram(&spawn);
    expect_status(outcome, status_code_t::invalid_argument_k);
    expect_true(executor == nullptr);
}

/** A CPU class the machine lacks caps the pick rather than refusing it: the database opens and
 *  describes a class other than the ceiling, while a ceiling at the machine's own pick keeps it. */
case_result_t test_database_cpu_class_ceiling(config_t const& config) {
    if (config.engine() != "ram") return case_result_t::skipped(needs_ram_engine_k);
    ustore_outcome_t outcome {};
    auto picked_under = [&](ustore_cpu_class_t cpu_class_ceiling) noexcept {
        ustore_database_t database = nullptr;
        ustore_database_init_ram_t init {.database = &database,
                                         .outcome = &outcome,
                                         .cpu_class_ceiling = cpu_class_ceiling};
        ustore_database_init_ram(&init);
        ustore_database_description_t description {};
        if (outcome.status != ustore_status_success_k) return description.cpu_class;
        ustore_database_describe_t describe {.database = database, .outcome = &outcome, .description = &description};
        ustore_database_describe(&describe);
        ustore_database_free(database);
        return description.cpu_class;
    };
    ustore_cpu_class_t const machine = picked_under(ustore_cpu_class_unknown_k);
    if (!expect_success(outcome)) return {};
    // A monoversioned build compiles no class under any other, so it refuses a lower ceiling
    if (machine == ustore_cpu_class_monoversioned_k)
        return case_result_t::skipped("built without USTORE_MULTIVERSIONED");
    expect_true(picked_under(machine) == machine);
    expect_success(outcome);

    // Power10 and AWS G5 share no architecture, so one of them is foreign to every machine
    ustore_cpu_class_t const foreign = machine == ustore_cpu_class_ibm_power10_k ? ustore_cpu_class_aws_g5_k
                                                                                 : ustore_cpu_class_ibm_power10_k;
    ustore_cpu_class_t const narrowed = picked_under(foreign);
    expect_success(outcome);
    expect_true(narrowed != foreign && narrowed != ustore_cpu_class_unknown_k);

    ustore_executor_t executor = nullptr;
    ustore_executor_for_ram_t spawn {.executor = &executor,
                                     .outcome = &outcome,
                                     .threads = 1,
                                     .cpu_class_ceiling = foreign};
    ustore_executor_for_ram(&spawn);
    expect_success(outcome);
    expect_true(executor != nullptr);
    ustore_executor_free(executor);
    return {};
}

/** A transaction reset names the database the transaction belongs to, so a reset through another
 *  database is refused before any engine runs, and the transaction keeps serving its own. */
case_result_t test_database_transaction_reset_elsewhere(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return {};
    if (!database.on().supports_transactions()) return case_result_t::skipped(needs_transactions_k);

    // The in-memory engine opens a second database without a directory, and a disk engine in a
    // directory of its own
    expected<database_t> other = status_t {status_code_t::uninitialized_k, "No second database"};
    if (config.engine() == "ram") other = open_database<database_t>(config_t {config.options(), nullptr});
    else if (config.directory()) {
        std::string const directory = fmt::format("{}/{}", config.directory(), "elsewhere");
        expect_true(scratch_directory_t::recreate(directory.c_str()));
        other = open_database<database_t>(config_t {config.options(), directory.c_str()});
    }
    else return case_result_t::skipped(needs_directory_k);
    if (!expect_true(other.has_value())) return {};

    ustore_outcome_t outcome {};
    ustore_transaction_t transaction = nullptr;
    ustore_transaction_init_t begin {.transaction = &transaction, .database = database, .outcome = &outcome};
    ustore_transaction_init(&begin);
    expect_success(outcome);
    ustore_transaction_t const begun = transaction;
    ustore_transaction_init_t reset_elsewhere {.transaction = &transaction, .database = *other, .outcome = &outcome};
    ustore_transaction_init(&reset_elsewhere);
    expect_status(outcome, status_code_t::invalid_argument_k);
    expect_true(transaction == begun);

    auto [blobs, blobs_status] = create_collection(database, "database_transaction_reset_elsewhere.blobs",
                                                   ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());
    ustore_key_t const key = key_from_integer(460);
    write_text(database, blobs, {&key, 1}, "value", transaction);
    ustore_transaction_commit_t commit {.database = database, .outcome = &outcome, .transaction = transaction};
    ustore_transaction_commit(&commit);
    expect_success(outcome);
    ustore_transaction_free(transaction);
    return {};
}

/** A small read answers a tape of 32-bit offsets, @c count + 1 of them, the first at zero and the
 *  last closing the final value, beside lengths where a missing key reads as empty. */
void test_database_read_offsets(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    auto [blobs, blobs_status] = create_collection(database, "database_read_offsets.blobs", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    expect_true(arena_status.succeeded());

    std::array<ustore_key_t, 3> const keys {key_from_integer(41), key_from_integer(42), key_from_integer(43)};
    write_text(database, blobs, {&keys[0], 1}, "a");
    write_text(database, blobs, {&keys[2], 1}, "bcd");
    read_columns_t columns;
    ustore_length_t const* lengths = nullptr;
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read = read_of(database, blobs, keys, columns, arena, outcome);
    read.lengths = &lengths;
    ustore_blobs_read(&read);
    expect_success(outcome);
    if (!expect_true(columns.presences && lengths && tape_written(columns.values))) return;

    expect_equal(columns.presences[0], 0b101);
    expect_true(columns.values.layout == ustore_strings_u32_tape_k);
    std::uint32_t const* const offsets = columns.values.data.u32_tape.offsets;
    expect_equal(offsets[0], 0u);
    expect_equal(offsets[1] - offsets[0], 1u);
    expect_equal(offsets[2] - offsets[1], 0u);
    expect_equal(offsets[3] - offsets[2], 3u);
    expect_true(lengths[0] == 1 && lengths[1] == 0 && lengths[2] == 3);
    expect_equal(columns.value(2), "bcd");
}

/** A u32 tape, a u64 tape and an accessor holding the same values, one of them empty, write
 *  the same bytes. */
void test_database_write_layouts(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    expect_true(open_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(arena_status.succeeded())) return;
    auto [blobs, blobs_status] = create_collection(database, "database_write_layouts.blobs", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());

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
        expect_success(outcome);

        read_columns_t columns;
        ustore_blobs_read_t read = read_of(database, blobs, keys, columns, arena, outcome);
        ustore_blobs_read(&read);
        expect_success(outcome);
        expect_true(columns.presences && tape_written(columns.values));
        if (!columns.presences || !tape_written(columns.values)) continue;
        expect_equal(columns.presences[0], 0b111);
        for (std::size_t index = 0; index != texts.size(); ++index) expect_equal(columns.value(index), texts[index]);
    }
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
    *accessor.largest_block = fu::max_of<std::size_t>(*accessor.largest_block, count);
    for (ustore_size_t index = 0; index != count; ++index) views[index] = bytes_of(accessor.value);
}

/** An accessor answering one value for every index broadcasts it to every key, asked in blocks of
 *  at most 64. */
void test_database_write_broadcast(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    expect_true(open_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(arena_status.succeeded())) return;
    auto [blobs, blobs_status] = create_collection(database, "database_write_broadcast.blobs", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());

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
    expect_success(outcome);
    expect_true(largest_block != 0 && largest_block <= 64);

    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, keys, columns, arena, outcome);
    ustore_blobs_read(&read);
    expect_success(outcome);
    expect_true(tape_written(columns.values));
    if (!tape_written(columns.values)) return;
    std::size_t differing = 0;
    for (std::size_t index = 0; index != count; ++index) differing += columns.value(index) != "same";
    expect_equal(differing, 0u);
}

/** A strings view in a layout no member of its union describes is refused
 *  with @c ustore_status_invalid_argument_k. */
void test_database_unknown_layout(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    auto [blobs, blobs_status] = create_collection(database, "database_unknown_layout.blobs", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());

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
    expect_status(outcome, status_code_t::invalid_argument_k);
}

/** A name or a directory with a length but no bytes is refused, never read through @c NULL. */
void test_database_lengths_without_bytes(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;

    ustore_collection_id_t collection {};
    ustore_outcome_t outcome {};
    ustore_collection_create_t create {.database = database,
                                       .outcome = &outcome,
                                       .name = {nullptr, 4},
                                       .modality = ustore_modality_blobs_k,
                                       .collection = &collection};
    ustore_collection_create(&create);
    expect_status(outcome, status_code_t::invalid_argument_k);

    ustore_database_t opened = nullptr;
    ustore_database_open_ram_t open {.database = &opened, .outcome = &outcome, .directory = {nullptr, 4}};
    ustore_database_open_ram(&open);
    expect_status(outcome, status_code_t::invalid_argument_k);
    expect_true(opened == nullptr);

    // A NUL inside a path would cut every file name made from it short
    std::string_view const cut_short {"store\0.ustore.json", 18};
    open.directory = bytes_of(cut_short);
    ustore_database_open_ram(&open);
    expect_status(outcome, status_code_t::invalid_argument_k);
    expect_true(opened == nullptr);
}

/** A tape whose strings span bytes it has no pointer to, or whose offsets step back, is refused,
 *  while a tape of empty strings needs none. */
void test_database_tape_without_bytes(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    auto [blobs, blobs_status] = create_collection(database, "database_tape_without_bytes.blobs",
                                                   ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());

    ustore_key_t const key = key_from_integer(1800);
    std::array<std::uint32_t, 2> const spanning_offsets {0, 8}, empty_offsets {0, 0};
    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {.database = database,
                                .outcome = &outcome,
                                .count = 1,
                                .collections = {&blobs, 0},
                                .keys = {&key, 0},
                                .values = u32_tape(nullptr, spanning_offsets.data())};
    ustore_blobs_write(&write);
    expect_status(outcome, status_code_t::invalid_argument_k);
    write.values = u32_tape(nullptr, empty_offsets.data());
    ustore_blobs_write(&write);
    expect_success(outcome);

    ustore_key_t const keys[] {key_from_integer(1801), key_from_integer(1802)};
    std::array<std::uint32_t, 3> const backward_offsets {0, 8, 4};
    char const bytes[8] {};
    write.count = 2;
    write.keys = {keys, sizeof(ustore_key_t)};
    write.values = u32_tape(reinterpret_cast<ustore_byte_t const*>(bytes), backward_offsets.data());
    ustore_blobs_write(&write);
    expect_status(outcome, status_code_t::invalid_argument_k);
}

/** A zeroed view of fields keeps its default, the whole document, on a write and on a read. */
void test_database_zeroed_fields(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    expect_true(open_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(arena_status.succeeded())) return;
    auto [blobs, blobs_status] = create_collection(database, "database_zeroed_fields.blobs", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());

    auto [docs, docs_status] = create_collection(database, "zeroed.docs", ustore_modality_docs_k);
    if (!expect_true(docs_status.succeeded())) return;

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
    expect_success(outcome);

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
    expect_success(outcome);
    if (!expect_true(columns.presences && tape_written(columns.values))) return;
    expect_equal(columns.presences[0] & 1, 1);
    expect_equal(columns.value(0), document);
}

/** The verbs of every modality refuse a collection holding another one, so a document's bytes and
 *  keys stay out of reach of the blobs verbs, documents are drawn by their own sample, and a blobs
 *  collection takes no documents. */
case_result_t test_database_modality_refusals(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(open_status.succeeded() && arena_status.succeeded())) return {};
    auto [blobs, blobs_status] = create_collection(database, "modality.blobs", ustore_modality_blobs_k);
    auto [docs, docs_status] = create_collection(database, "modality.docs", ustore_modality_docs_k);
    auto [graph, graph_status] = create_collection(database, "modality.graph", ustore_modality_graph_k);
    if (!expect_true(blobs_status.succeeded() && docs_status.succeeded() && graph_status.succeeded())) return {};

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
    expect_success(outcome);

    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, {&key, 1}, columns, arena, outcome);
    read.collections = {&docs, 0};
    ustore_blobs_read(&read);
    expect_status(status_t {outcome}, status_code_t::invalid_argument_k);

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
    expect_success(outcome);
    expect_true(offsets && drawn && offsets[1] == 1 && drawn[0] == key);
    sample_documents.collections = {&blobs, 0};
    ustore_docs_sample(&sample_documents);
    expect_status(status_t {outcome}, status_code_t::invalid_argument_k);
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
    expect_status(status_t {outcome}, status_code_t::invalid_argument_k);

    write_document.collections = {&blobs, 0};
    ustore_docs_write(&write_document);
    expect_status(status_t {outcome}, status_code_t::invalid_argument_k);

    ustore_graph_write_edges_t write_edge {
        .database = database,
        .outcome = &outcome,
        .count = 1,
        .collections = {&docs, 0},
        .sources = {&key, 0},
        .targets = {&key, 0},
    };
    ustore_graph_write_edges(&write_edge);
    expect_status(status_t {outcome}, status_code_t::invalid_argument_k);

    ustore_pages_import_t import {};
    import.database = database, import.outcome = &outcome, import.count = 1, import.collections = {&graph, 0};
    import.keys = {&key, 0}, import.values = documents.view(), import.encoding = ustore_page_encoding_xml_k;
    ustore_pages_import(&import);
    expect_status(status_t {outcome}, status_code_t::invalid_argument_k);
    return {};
}

/** The documents, the vertices and the pages each list their own keys, a vertex without
 *  edges included, and a document removal takes its key away, while no modality lists the
 *  keys of another. */
case_result_t test_database_modality_keys(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(open_status.succeeded() && arena_status.succeeded())) return {};
    auto [docs, docs_status] = create_collection(database, "keys.docs", ustore_modality_docs_k);
    auto [graph, graph_status] = create_collection(database, "keys.graph", ustore_modality_graph_k);
    auto [pages, pages_status] = create_collection(database, "keys.pages", ustore_modality_pages_k);
    if (!expect_true(docs_status.succeeded() && graph_status.succeeded() && pages_status.succeeded())) return {};

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
    expect_success(outcome);
    ustore_graph_write_vertices_t write_vertices {.database = database,
                                                  .outcome = &outcome,
                                                  .count = keys.size(),
                                                  .collections = {&graph, 0},
                                                  .vertices = {keys.data(), sizeof(ustore_key_t)}};
    ustore_graph_write_vertices(&write_vertices);
    expect_success(outcome);
    std::string_view const markup = "<a/>";
    strided_strings<std::string_view> const markups {{&markup, 0}};
    ustore_pages_import_t import {};
    import.database = database, import.outcome = &outcome, import.count = keys.size(), import.collections = {&pages, 0};
    import.keys = {keys.data(), sizeof(ustore_key_t)}, import.values = markups.view();
    import.encoding = ustore_page_encoding_xml_k;
    ustore_pages_import(&import);
    expect_success(outcome);

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
        if (refused != status_code_t::success_k) {
            expect_status(outcome, refused);
            return;
        }
        if (!expect_success(outcome) || !expect_true(offsets != nullptr)) return;
        std::span<ustore_key_t const> const listed {found + offsets[0], found + offsets[1]};
        expect_true(std::equal(listed.begin(), listed.end(), wanted.begin(), wanted.end()));
    };
    expect_listed(&ustore_docs_scan, docs, keys);
    expect_listed(&ustore_graph_scan_vertices, graph, keys);
    expect_listed(&ustore_pages_scan, pages, keys);
    expect_listed(&ustore_blobs_scan, docs, {}, status_code_t::invalid_argument_k);
    expect_listed(&ustore_docs_scan, graph, {}, status_code_t::invalid_argument_k);
    expect_listed(&ustore_graph_scan_vertices, pages, {}, status_code_t::invalid_argument_k);
    expect_listed(&ustore_pages_scan, docs, {}, status_code_t::invalid_argument_k);

    ustore_docs_remove_t remove {.database = database,
                                 .outcome = &outcome,
                                 .count = 1,
                                 .collections = {&docs, 0},
                                 .keys = {keys.data(), sizeof(ustore_key_t)}};
    ustore_docs_remove(&remove);
    expect_success(outcome);
    expect_listed(&ustore_docs_scan, docs, std::span {keys}.subspan(1));
    remove.collections = {&graph, 0};
    ustore_docs_remove(&remove);
    expect_status(status_t {outcome}, status_code_t::invalid_argument_k);
    return {};
}

/** A strings write takes its strings as a u64 tape and its keys as a strided view, and a read
 *  naming the same strings by a u32 tape finds every key. */
case_result_t test_database_strings_layouts(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    expect_true(open_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(arena_status.succeeded())) return {};
    auto [strings, strings_status] = create_collection(database, "layouts.strings", ustore_modality_strings_k);
    if (!expect_true(strings_status.succeeded())) return {};

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
    expect_success(outcome);

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
    expect_success(outcome);
    if (!expect_true(presences && values)) return {};
    expect_equal(presences[0], 0b11);
    expect_equal(values[0], keys[0]);
    expect_equal(values[1], keys[1]);
    return {};
}

/** A measurement without start or end keys spans the whole collection on every engine. */
void test_database_measure_whole(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    auto [blobs, blobs_status] = create_collection(database, "database_measure_whole.blobs", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    expect_true(arena_status.succeeded());

    ustore_key_t const key = key_from_integer(950);
    write_text(database, blobs, {&key, 1}, "value");
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
    expect_success(outcome);
    expect_true(min_cardinalities && max_cardinalities);
    if (min_cardinalities && max_cardinalities) expect_true(min_cardinalities[0] <= max_cardinalities[0]);
}

/** A removal takes away the keys it names and leaves the ones a write beside it stored. */
void test_database_remove(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return;
    auto [blobs, blobs_status] = create_collection(database, "database_remove.blobs", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    expect_true(arena_status.succeeded());

    std::array<ustore_key_t, 3> const keys {key_from_integer(51), key_from_integer(52), key_from_integer(53)};
    write_text(database, blobs, keys, "value");

    // The first and the last key go in one batch, which strides over the middle one
    ustore_outcome_t outcome {};
    ustore_blobs_remove_t remove {};
    remove.database = database;
    remove.outcome = &outcome;
    remove.count = 2;
    remove.collections = {&blobs, 0};
    remove.keys = {keys.data(), 2 * sizeof(ustore_key_t)};
    ustore_blobs_remove(&remove);
    expect_success(outcome);

    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, keys, columns, arena, outcome);
    ustore_blobs_read(&read);
    expect_success(outcome);
    expect_true(columns.presences && columns.presences[0] == 0b010);
}

/** A rollback withdraws a stage but keeps the writes, so the next commit publishes them. */
case_result_t test_database_rollback(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return {};
    auto [blobs, blobs_status] = create_collection(database, "database_rollback.blobs", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());
    if (!database.on().supports_transactions()) return case_result_t::skipped(needs_transactions_k);
    expected<transaction_t> maybe_transaction = database.on().transaction();
    if (!expect_true(maybe_transaction.has_value())) return {};
    ustore_transaction_t const transaction = maybe_transaction->transaction();

    ustore_key_t const key = key_from_integer(61);
    write_text(database, blobs, {&key, 1}, "staged", transaction);
    ustore_outcome_t outcome {};
    ustore_transaction_stage_t stage {.database = database, .outcome = &outcome, .transaction = transaction};
    ustore_transaction_stage(&stage);
    expect_success(outcome);
    ustore_transaction_rollback_t rollback {.database = database, .outcome = &outcome, .transaction = transaction};
    ustore_transaction_rollback(&rollback);
    expect_success(outcome);
    ustore_transaction_commit_t commit {.database = database, .outcome = &outcome, .transaction = transaction};
    ustore_transaction_commit(&commit);
    expect_success(outcome);

    auto [arena, arena_status] = arena_t::make();
    expect_true(arena_status.succeeded());
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, {&key, 1}, columns, arena, outcome);
    ustore_blobs_read(&read);
    expect_success(outcome);
    expect_true(tape_written(columns.values) && columns.value(0) == "staged");
    return {};
}

/** A stage reads at a stamp no later than the commit after it, and a snapshot taken next reads at
 *  that commit or past it. */
case_result_t test_database_stamps(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return {};
    auto [blobs, blobs_status] = create_collection(database, "database_stamps.blobs", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());
    if (!database.on().supports_transactions() || !database.on().supports_snapshots())
        return case_result_t::skipped(needs_transactions_and_snapshots_k);

    // A commit at the head first, so the stage reads above zero and a commit reporting nothing
    // reads below it
    ustore_key_t const key = key_from_integer(62);
    write_text(database, blobs, {&key, 1}, "head");
    expected<transaction_t> maybe_transaction = database.on().transaction();
    if (!expect_true(maybe_transaction.has_value())) return {};
    ustore_transaction_t const transaction = maybe_transaction->transaction();
    write_text(database, blobs, {&key, 1}, "staged", transaction);

    ustore_stamp_t read_stamp = (std::numeric_limits<ustore_stamp_t>::max)();
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
    expect_success(outcome);
    ustore_transaction_commit_t commit {
        .database = database,
        .outcome = &outcome,
        .transaction = transaction,
        .commit_stamp = &commit_stamp,
    };
    ustore_transaction_commit(&commit);
    expect_success(outcome);
    expect_true(read_stamp <= commit_stamp);

    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t snapshot_init {
        .snapshot = &snapshot,
        .database = database,
        .outcome = &outcome,
        .read_stamp = &snapshot_stamp,
    };
    ustore_snapshot_init(&snapshot_init);
    expect_success(outcome);
    expect_true(commit_stamp <= snapshot_stamp);
    ustore_snapshot_free(snapshot);
    return {};
}

/** Freeing a database before its transaction and snapshot leaves both working, and freeing the last
 *  of them closes the database, so it opens again. */
case_result_t test_database_free_holds(config_t const& config) {
    auto [database, open_status] = open_raw(config);
    if (!expect_true(open_status.succeeded())) return {};

    ustore_outcome_t outcome {};
    ustore_database_description_t description {};
    ustore_database_describe_t describe {.database = database, .outcome = &outcome, .description = &description};
    ustore_database_describe(&describe);
    expect_success(outcome);
    constexpr int needed_k = ustore_capabilities_transactions_k | ustore_capabilities_snapshots_k;
    if ((description.capabilities & needed_k) != needed_k) {
        ustore_database_free(database);
        return case_result_t::skipped(needs_transactions_and_snapshots_k);
    }
    auto [blobs, blobs_status] = create_collection(database, "database_free_holds.blobs", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());

    ustore_transaction_t transaction = nullptr;
    ustore_transaction_init_t transaction_init {.transaction = &transaction, .database = database, .outcome = &outcome};
    ustore_transaction_init(&transaction_init);
    expect_success(outcome);
    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t snapshot_init {.snapshot = &snapshot, .database = database, .outcome = &outcome};
    ustore_snapshot_init(&snapshot_init);
    expect_success(outcome);
    ustore_database_free(database);

    ustore_key_t const key = key_from_integer(90);
    write_text(database, blobs, {&key, 1}, "held", transaction);
    ustore_transaction_commit_t commit {.database = database, .outcome = &outcome, .transaction = transaction};
    ustore_transaction_commit(&commit);
    expect_success(outcome);

    // The snapshot predates the commit, so it still misses the key
    auto [arena, arena_status] = arena_t::make();
    expect_true(arena_status.succeeded());
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, {&key, 1}, columns, arena, outcome);
    read.view = ustore_snapshot_view(snapshot);
    ustore_blobs_read(&read);
    expect_success(outcome);
    expect_true(columns.presences && (columns.presences[0] & 1) == 0);

    ustore_transaction_free(transaction);
    ustore_snapshot_free(snapshot);
    auto [reopened, reopen_status] = open_raw(config);
    expect_true(reopen_status.succeeded());
    ustore_database_free(reopened);
    return {};
}

/** A snapshot opened before its collection is dropped still reads that collection's values,
 *  where the engine lets the drop go ahead, while the collection is gone at the head. */
case_result_t test_snapshot_reads_collection_dropped_after_open(config_t const& config) {
    auto [database, open_status] = open_raw(config);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(open_status.succeeded() && arena_status.succeeded())) return {};

    ustore_outcome_t outcome {};
    ustore_database_description_t description {};
    ustore_database_describe_t describe {.database = database, .outcome = &outcome, .description = &description};
    ustore_database_describe(&describe);
    expect_success(outcome);
    if ((description.capabilities & ustore_capabilities_snapshots_k) == 0) {
        ustore_database_free(database);
        return case_result_t::skipped(needs_snapshots_k);
    }
    auto [blobs, blobs_status] = create_collection(database, "snapshot_dropped.blobs", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());
    ustore_key_t const key = key_from_integer(91);
    write_text(database, blobs, {&key, 1}, "kept");

    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t snapshot_init {.snapshot = &snapshot, .database = database, .outcome = &outcome};
    ustore_snapshot_init(&snapshot_init);
    expect_success(outcome);
    ustore_collection_drop_t drop {.database = database,
                                   .outcome = &outcome,
                                   .collection = blobs,
                                   .mode = ustore_drop_collection_k};
    ustore_collection_drop(&drop);
    bool const dropped = outcome.status == ustore_status_success_k;
    if (!dropped) expect_status(outcome, status_code_t::invalid_argument_k);

    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, {&key, 1}, columns, arena, outcome);
    read.view = ustore_snapshot_view(snapshot);
    ustore_blobs_read(&read);
    expect_success(outcome);
    expect_true(columns.presences && (columns.presences[0] & 1) == 1 && columns.value(0) == "kept");
    if (dropped)
        expect_status(collection_named(receiver_t {database}, "snapshot_dropped.blobs"), status_code_t::not_found_k);
    ustore_snapshot_free(snapshot);
    ustore_database_free(database);
    return {};
}

/** A fresh database holds no collection until one is created, never listing its system collections,
 *  and refuses a name starting with an underscore, which they keep. */
case_result_t test_database_fresh_collections(config_t const& config) {
    expected<database_t> fresh = status_t {status_code_t::uninitialized_k, "No fresh database"};
    if (config.engine() == "ram") fresh = open_database<database_t>(config_t {config.options(), nullptr});
    else if (config.directory()) {
        std::string const directory = fmt::format("{}/{}", config.directory(), "fresh");
        expect_true(scratch_directory_t::recreate(directory.c_str()));
        fresh = open_database<database_t>(config_t {config.options(), directory.c_str()});
    }
    else return case_result_t::skipped(needs_directory_k);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(fresh.has_value() && arena_status.succeeded())) return {};

    auto [listed, list_status] = fresh->on().collections(arena);
    expect_true(list_status.succeeded());
    expect_equal(listed.size(), 0u);
    expect_status(collection_named(fresh->receiver(), "_names"), status_code_t::not_found_k);
    expect_status(create_collection(*fresh, "_x", ustore_modality_blobs_k), status_code_t::invalid_argument_k);
    return {};
}

/** A name resolves at the snapshot it is read at, so a collection created after the snapshot
 *  stays missing there. */
case_result_t test_database_find_at_snapshot(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return {};
    if (!database.on().supports_snapshots()) return case_result_t::skipped(needs_snapshots_k);

    expected<snapshot_t> snapshot = database.on().snapshot();
    if (!expect_true(snapshot.has_value())) return {};
    expect_true(create_collection(database, "later", ustore_modality_blobs_k).has_value());
    auto [arena, arena_status] = arena_t::make();
    expect_true(arena_status.succeeded());

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
    expect_success(outcome);
    expect_true(presences && found && presences[0] == 1);

    find.snapshot = snapshot->snapshot();
    ustore_collection_find(&find);
    expect_success(outcome);
    expect_true(presences && found && presences[0] == 0);
    if (found) expect_true(found[0] == ustore_collection_id_t {});
    return {};
}

/** Fails unless exactly @p count values of a byte have a spelling, each reading back as its value,
 *  while a name no enumerator holds is refused and leaves the output as it was. */
template <typename enum_type_>
void expect_spellings(char const* (*name_of)(enum_type_),
                      ustore_status_t (*from_name)(ustore_bytes_view_t, enum_type_*), std::size_t count) {
    std::size_t spelled = 0;
    enum_type_ read {};
    // Values no enumerator holds reach the library as a C caller passes them, never loaded as
    // the enum here
    for (int value = 0; value != 256; ++value) {
        std::string_view const name = name_of(static_cast<enum_type_>(value));
        if (name == "unrecognized") continue;
        ++spelled;
        expect_true(from_name(bytes_of(name), &read) == ustore_status_success_k);
        expect_true(static_cast<int>(read) == value);
    }
    expect_equal(spelled, count);
    enum_type_ const kept = read;
    expect_true(from_name(bytes_of(std::string_view {"bogus"}), &read) == ustore_status_invalid_argument_k);
    expect_true(read == kept);
    expect_true(from_name(bytes_of(std::string_view {"bogus"}), nullptr) == ustore_status_invalid_argument_k);
}

/** Every status spelling reads back as its status, while a name no status holds is refused. */
void test_status_names(config_t const&) {
    expect_spellings(ustore_status_name, ustore_status_from_name, 19);
    expect_true(std::string_view {ustore_status_name(ustore_status_deferred_k)} == "deferred");
    expect_true(std::string_view {ustore_status_name(ustore_status_read_only_k)} == "read-only");
}

/** Every closed set spells each of its enumerators, reads every spelling back and refuses the
 *  rest. */
void test_closed_set_names(config_t const&) {
    expect_spellings(ustore_cpu_class_name, ustore_cpu_class_from_name, 12);
    expect_spellings(ustore_gpu_vendor_name, ustore_gpu_vendor_from_name, 3);
    expect_spellings(ustore_gpu_class_name, ustore_gpu_class_from_name, 11);
    expect_spellings(ustore_mood_name, ustore_mood_from_name, 2);
    expect_spellings(ustore_durability_name, ustore_durability_from_name, 2);
    expect_spellings(ustore_watch_name, ustore_watch_from_name, 2);
    expect_spellings(ustore_scan_mode_name, ustore_scan_mode_from_name, 2);
    expect_spellings(ustore_scan_direction_name, ustore_scan_direction_from_name, 2);
    expect_spellings(ustore_change_kind_name, ustore_change_kind_from_name, 3);
    expect_spellings(ustore_drop_mode_name, ustore_drop_mode_from_name, 3);
    expect_spellings(ustore_isolation_name, ustore_isolation_from_name, 2);
    expect_spellings(ustore_transport_name, ustore_transport_from_name, 5);
    expect_spellings(ustore_gds_name, ustore_gds_from_name, 3);
    expect_spellings(ustore_pattern_kind_name, ustore_pattern_kind_from_name, 2);
    expect_spellings(ustore_case_sensitivity_name, ustore_case_sensitivity_from_name, 2);
    expect_spellings(ustore_predicate_cost_name, ustore_predicate_cost_from_name, 2);
    expect_spellings(ustore_engine_name, ustore_engine_from_name, 4);
    expect_spellings(ustore_modality_name, ustore_modality_from_name, 5);
    expect_spellings(ustore_vertex_role_name, ustore_vertex_role_from_name, 4);
    expect_spellings(ustore_docs_field_type_name, ustore_docs_field_type_from_name, 17);
    expect_spellings(ustore_docs_modification_name, ustore_docs_modification_from_name, 5);
    expect_spellings(ustore_page_encoding_name, ustore_page_encoding_from_name, 5);
    expect_true(std::string_view {ustore_isolation_name(ustore_isolation_strict_serializable_k)} ==
                "strict-serializable");
}

/** A description names the engine that opened the database, and the RAM engine its isolation and
 *  its keys, while a compaction keeps every key. */
void test_database_describe_compact(config_t const& config) {
    auto [database, open_status] = open_raw(config);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(open_status.succeeded() && arena_status.succeeded())) return;
    auto [blobs, blobs_status] = create_collection(database, "database_describe_compact.blobs",
                                                   ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());
    std::array<ustore_key_t, 3> const keys {key_from_integer(31), key_from_integer(32), key_from_integer(33)};
    write_text(database, blobs, keys, "kept");

    ustore_outcome_t outcome {};
    ustore_database_description_t description {};
    ustore_database_describe_t describe {.database = database, .outcome = &outcome, .description = &description};
    ustore_database_describe(&describe);
    expect_success(outcome);
    expect_true(std::string_view {ustore_engine_name(description.engine)} == config.engine());
    if (config.engine() == "ram") {
        expect_true(std::string_view {ustore_isolation_name(description.isolation)} == config.options().isolation);
        expect_true(description.keys >= keys.size());
    }

    ustore_database_compact_t compact {.database = database, .outcome = &outcome};
    ustore_database_compact(&compact);
    expect_success(outcome);
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, keys, columns, arena, outcome);
    ustore_blobs_read(&read);
    expect_success(outcome);
    expect_true(columns.presences && columns.presences[0] == 7 && columns.value(2) == "kept");
    ustore_database_free(database);
}

/** A clear empties every value keeping the keys, then removes every pair keeping the collections,
 *  then drops every collection, as each mode asks. */
void test_database_clear_modes(config_t const& config) {
    auto [database, open_status] = open_raw(config);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(open_status.succeeded() && arena_status.succeeded())) return;
    auto [blobs, blobs_status] = create_collection(database, "clear.kept", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());
    ustore_key_t const key = key_from_integer(77);
    write_text(database, blobs, {&key, 1}, "value");

    ustore_outcome_t outcome {};
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, {&key, 1}, columns, arena, outcome);
    ustore_database_clear_t clear {.database = database, .outcome = &outcome, .mode = ustore_drop_values_k};
    ustore_database_clear(&clear);
    expect_success(outcome);
    ustore_blobs_read(&read);
    expect_success(outcome);
    expect_true(columns.presences && columns.presences[0] == 1 && columns.value(0).empty());

    clear.mode = ustore_drop_pairs_k;
    ustore_database_clear(&clear);
    expect_success(outcome);
    ustore_blobs_read(&read);
    expect_success(outcome);
    expect_true(columns.presences && columns.presences[0] == 0);
    expect_true(collection_named(receiver_t {database}, "clear.kept").has_value());

    // A snapshot belongs to its holder, so the RAM and RocksDB engines drop no collection under it
    auto [pinned, pinned_status] = create_collection(database, "clear.pinned", ustore_modality_blobs_k);
    expect_true(pinned_status.succeeded());
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
        refuses ? expect_status(outcome, status_code_t::invalid_argument_k) : expect_success(outcome);
        ustore_snapshot_free(snapshot);
    }

    clear.mode = ustore_drop_collection_k;
    ustore_database_clear(&clear);
    expect_success(outcome);
    expect_status(collection_named(receiver_t {database}, "clear.kept"), status_code_t::not_found_k);
    ustore_database_free(database);
}

/** A database RocksDB opens read-only serves reads, snapshots and their saves, and refuses every
 *  kind of call that would change it with @c ustore_status_read_only_k. */
case_result_t test_database_view_refusals(config_t const& config) {
    if (config.engine() != "rocksdb") return case_result_t::skipped("needs the rocksdb engine");
    if (!config.directory()) return case_result_t::skipped(needs_directory_k);
    std::string const directory = fmt::format("{}/{}", config.directory(), "viewed");
    std::string const saved = fmt::format("{}/{}", config.directory(), "viewed.saved");
    expect_true(scratch_directory_t::recreate(directory.c_str()) && scratch_directory_t::recreate(saved.c_str()));
    ustore_bytes_view_t const path = bytes_of(std::string_view {directory.c_str()});
    ustore_key_t const key = key_from_integer(5);
    ustore_outcome_t outcome {};
    ustore_database_t database = nullptr;
    ustore_database_open_rocksdb_t open {.database = &database, .outcome = &outcome, .directory = path};
    ustore_database_open_rocksdb(&open);
    if (!expect_success(outcome)) return {};
    auto [blobs, blobs_status] = create_collection(database, "view.blobs", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());
    write_text(database, blobs, {&key, 1}, "viewed");
    auto [created, create_status] = create_collection(database, "view.kept", ustore_modality_blobs_k);
    expect_true(create_status.succeeded());
    ustore_database_free(database);

    ustore_database_view_rocksdb_t view {.database = &database, .outcome = &outcome, .directory = path};
    ustore_database_view_rocksdb(&view);
    expect_success(outcome);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(arena_status.succeeded())) return {};
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, {&key, 1}, columns, arena, outcome);
    ustore_blobs_read(&read);
    expect_success(outcome);
    expect_true(tape_written(columns.values) && columns.value(0) == "viewed");
    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t snapshot_init {.snapshot = &snapshot, .database = database, .outcome = &outcome};
    ustore_snapshot_init(&snapshot_init);
    expect_success(outcome);
    std::string const target = save_target(config, saved);
    ustore_snapshot_save_t save {.snapshot = snapshot,
                                 .outcome = &outcome,
                                 .path = bytes_of(std::string_view {target.c_str()})};
    ustore_snapshot_save(&save);
    expect_success(outcome);
    ustore_snapshot_free(snapshot);

    ustore_blobs_write_t write {.database = database,
                                .outcome = &outcome,
                                .count = 1,
                                .collections = {&blobs, 0},
                                .keys = {&key, 0},
                                .values = stored_values_k.view()};
    ustore_blobs_write(&write);
    expect_status(status_t {outcome}, status_code_t::read_only_k);
    ustore_blobs_remove_t remove {.database = database,
                                  .outcome = &outcome,
                                  .count = 1,
                                  .collections = {&blobs, 0},
                                  .keys = {&key, 0}};
    ustore_blobs_remove(&remove);
    expect_status(status_t {outcome}, status_code_t::read_only_k);
    ustore_transaction_t transaction = nullptr;
    ustore_transaction_init_t begin {.transaction = &transaction, .database = database, .outcome = &outcome};
    ustore_transaction_init(&begin);
    expect_status(status_t {outcome}, status_code_t::read_only_k);
    expect_true(transaction == nullptr);
    expect_status(create_collection(database, "view.created", ustore_modality_blobs_k), status_code_t::read_only_k);
    ustore_collection_drop_t drop {.database = database, .outcome = &outcome, .collection = created};
    ustore_collection_drop(&drop);
    expect_status(status_t {outcome}, status_code_t::read_only_k);
    ustore_database_clear_t clear {.database = database, .outcome = &outcome, .mode = ustore_drop_pairs_k};
    ustore_database_clear(&clear);
    expect_status(status_t {outcome}, status_code_t::read_only_k);
    ustore_database_compact_t compact {.database = database, .outcome = &outcome};
    ustore_database_compact(&compact);
    expect_status(status_t {outcome}, status_code_t::read_only_k);
    auto [executor, spawn_status] = spawn_executor();
    expect_true(spawn_status.succeeded());
    ustore_database_compact_plan_t plan = nullptr;
    ustore_phase_progress_t phase {.phase = ustore_phase_done_k};
    compact.executor = executor;
    compact.plan = &plan;
    compact.phase = &phase;
    ustore_database_compact(&compact);
    expect_status(status_t {outcome}, status_code_t::read_only_k);
    expect_true(plan == nullptr && phase.phase == ustore_phase_none_k);
    ustore_executor_free(executor);
    ustore_database_free(database);
    return {};
}

/** A view of an NVMe database reads what its manifest names and refuses writes, while its open and
 *  its close leave the manifest's bytes and modification time as they were. */
case_result_t test_database_view_nvme(config_t const& config) {
    if (config.engine() != "nvme") return case_result_t::skipped("needs the nvme engine");
    if (!config.directory()) return case_result_t::skipped(needs_directory_k);
    std::string const directory = fmt::format("{}/{}", config.directory(), "viewed");
    expect_true(scratch_directory_t::recreate(directory.c_str()));
    engine_options_t options = config.options();
    options.locate(directory.c_str());
    char const* const manifest = options.nvme_manifest.data();
    ustore_key_t const key = key_from_integer(5);
    ustore_outcome_t outcome {};
    ustore_database_t database = nullptr;
    open_on_own_executor(options, &database, &outcome);
    if (!expect_success(outcome)) return {};
    auto [blobs, blobs_status] = create_collection(database, "view.blobs", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());
    write_text(database, blobs, {&key, 1}, "viewed");
    ustore_database_free(database);

    st::vector<char> before, after;
    expect_true(utilities::read_file(manifest, before).succeeded());
    std::error_code error;
    std::filesystem::file_time_type const modified = std::filesystem::last_write_time(manifest, error);
    expect_true(!error);

    database = nullptr;
    ustore_database_view_nvme_t view {.database = &database,
                                      .outcome = &outcome,
                                      .directory = bytes_of(std::string_view {manifest})};
    ustore_database_view_nvme(&view);
    if (!expect_success(outcome)) return {};
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(arena_status.succeeded())) return {};
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, {&key, 1}, columns, arena, outcome);
    ustore_blobs_read(&read);
    expect_success(outcome);
    expect_true(tape_written(columns.values) && columns.value(0) == "viewed");
    ustore_blobs_write_t write {.database = database,
                                .outcome = &outcome,
                                .count = 1,
                                .collections = {&blobs, 0},
                                .keys = {&key, 0},
                                .values = stored_values_k.view()};
    ustore_blobs_write(&write);
    expect_status(status_t {outcome}, status_code_t::read_only_k);
    ustore_database_flush_t flush {.database = database, .outcome = &outcome};
    ustore_database_flush(&flush);
    expect_status(status_t {outcome}, status_code_t::read_only_k);
    ustore_database_free(database);

    expect_true(utilities::read_file(manifest, after).succeeded());
    expect_true(before.size() == after.size() && std::equal(before.begin(), before.end(), after.begin()));
    expect_true(std::filesystem::last_write_time(manifest, error) == modified && !error);

    std::string const missing = fmt::format("{}/absent.ustore.json", directory);
    view.directory = bytes_of(std::string_view {missing.c_str()});
    ustore_database_view_nvme(&view);
    expect_status(status_t {outcome}, status_code_t::not_found_k);
    expect_true(!std::filesystem::exists(missing, error));
    return {};
}

/** A RAM database refuses with capacity_exhausted the write that would pass its memory limit,
 *  keeps what it landed before, and takes writes again after a clear, while LevelDB refuses to
 *  open with a second background thread. */
case_result_t test_database_open_budgets(config_t const& config) {
    if (config.engine() == "leveldb") {
        if (!config.directory()) return case_result_t::skipped(needs_directory_k);
        ustore_database_t database = nullptr;
        ustore_outcome_t outcome {};
        ustore_database_open_leveldb_t open {.database = &database,
                                             .outcome = &outcome,
                                             .directory = bytes_of(std::string_view {config.directory()}),
                                             .background_threads = 2};
        ustore_database_open_leveldb(&open);
        expect_status(status_t {outcome}, status_code_t::unsupported_k);
        expect_true(database == nullptr);
        return {};
    }
    if (config.engine() != "ram") return case_result_t::skipped(needs_ram_engine_k);
    ustore_database_t database = nullptr;
    ustore_outcome_t outcome {};
    ustore_database_init_ram_t init {.database = &database, .outcome = &outcome, .memory_limit = 64 * 1024};
    ustore_database_init_ram(&init);
    if (!expect_success(outcome)) return {};
    auto [blobs, blobs_status] = create_collection(database, "budgets.blobs", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());
    std::string const kilobyte(1024, 'k');
    std::array<ustore_key_t, 32> keys {};
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = key_from_integer(index);
    expect_true(write_text(database, blobs, keys, kilobyte));
    std::array<ustore_key_t, 48> more {};
    for (std::size_t index = 0; index != more.size(); ++index) more[index] = key_from_integer(1000 + index);
    std::string_view const kilobyte_view {kilobyte};
    strided_strings<std::string_view> const values {{&kilobyte_view, 0}};
    ustore_blobs_write_t write {.database = database,
                                .outcome = &outcome,
                                .count = more.size(),
                                .collections = {&blobs, 0},
                                .keys = {more.data(), sizeof(ustore_key_t)},
                                .values = values.view()};
    ustore_blobs_write(&write);
    expect_status(status_t {outcome}, status_code_t::capacity_exhausted_k);
    ustore_database_clear_t clear {.database = database, .outcome = &outcome, .mode = ustore_drop_pairs_k};
    ustore_database_clear(&clear);
    expect_success(outcome);
    ustore_blobs_write(&write);
    expect_success(outcome);
    ustore_database_free(database);
    return {};
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
case_result_t test_progress_cancel_scans(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(open_status.succeeded() && arena_status.succeeded())) return {};
    auto [blobs, blobs_status] = create_collection(database, "progress_cancel_scans.blobs", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());
    std::array<ustore_key_t, 3> const keys {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
    write_text(database, blobs, keys, "scanned");

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
    expect_status(status_t {outcome}, status_code_t::cancelled_k);
    expect_true(offsets == nullptr && found == nullptr);
    expect_equal(record.calls, 1u);
    expect_equal(record.total, 3u);

    auto [strings, strings_status] = create_collection(database, "progress.strings", ustore_modality_strings_k);
    if (!expect_true(strings_status.succeeded())) return {};
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
    expect_success(outcome);
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
    expect_status(status_t {outcome}, status_code_t::cancelled_k);
    expect_true(offsets == nullptr && !tape_written(texts));
    expect_equal(record.calls, 1u);
    return {};
}

/** A pull its callback cancels publishes nothing and leaves the cursor where it was, so the next
 *  pull repeats it. */
case_result_t test_progress_cancel_cursor(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(open_status.succeeded() && arena_status.succeeded())) return {};
    auto [collection, create_status] = create_collection(database, "progress.cursor", ustore_modality_blobs_k);
    expect_true(create_status.succeeded());
    std::array<ustore_key_t, 3> const keys {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
    write_text(database, collection, keys, "pulled");
    auto [cursor, cursor_status] = open_cursor(database, collection);
    if (!expect_true(cursor_status.succeeded())) return {};

    ustore_outcome_t outcome {};
    progress_record_t record {.cancelling_report = 1};
    pulled_keys_t pulled;
    ustore_blobs_cursor_next_t next = pull_of(database, cursor, pulled, arena, 2, outcome);
    next.progress = &record_progress;
    next.progress_state = &record;
    ustore_blobs_cursor_next(&next);
    expect_status(status_t {outcome}, status_code_t::cancelled_k);
    expect_true(pulled.keys == nullptr && pulled.written == 0 && !tape_written(pulled.values));
    expect_true(record.calls == 1 && record.finished == 1 && record.total == 1);

    next.progress = nullptr;
    ustore_blobs_cursor_next(&next);
    expect_success(outcome);
    expect_page(pulled, {keys.data(), 2}, "pulled");
    ustore_blobs_cursor_free(cursor);
    return {};
}

/** Whether the saved copy at @p path is gone: no image in the directory the RAM engine saves into,
 *  and no database where RocksDB and LevelDB create one. */
bool nothing_saved(config_t const& config, char const* path) noexcept {
    std::error_code error;
    if (config.engine() == "ram")
        return !std::filesystem::exists(fmt::format("{}/{}", path, "ram.image").c_str(), error) &&
               !std::filesystem::exists(fmt::format("{}/{}", path, "ram.image.tmp").c_str(), error);
    return !std::filesystem::exists(path, error) || std::filesystem::is_empty(path, error);
}

/** Readies @p path for a save: a fresh directory the RAM engine saves into, or no path where the
 *  others create it. */
bool ready_save_path(config_t const& config, char const* path) noexcept {
    if (config.engine() == "ram") return scratch_directory_t::recreate(path);
    std::error_code error;
    std::filesystem::remove_all(path, error);
    return !error;
}

/** Opens the copy saved at @p path with the engine of @p config. */
expected<ustore_database_t> open_saved(config_t const& config, char const* path) noexcept {
    engine_options_t options = config.options();
    options.locate(path);
    ustore_database_t database = nullptr;
    ustore_outcome_t outcome {};
    open_on_own_executor(options, &database, &outcome);
    if (status_t const status {outcome}; status.failed()) return status;
    return database;
}

/** A write asking for no durability reaches the files once @c ustore_database_flush returns, so a
 *  copy of them taken with the database still open, as a crash would leave them, reads it. */
case_result_t test_database_flush_durable(config_t const& config) {
    if (!config.directory()) return case_result_t::skipped(needs_directory_k);
    std::string const directory = fmt::format("{}/{}", config.directory(), "flushed");
    std::string const copied = fmt::format("{}/{}", config.directory(), "flushed.copy");
    expect_true(scratch_directory_t::recreate(directory.c_str()));
    auto [database, open_status] = open_saved(config, directory.c_str());
    if (!expect_true(open_status.succeeded())) return {};
    auto [blobs, blobs_status] = create_collection(database, "flushed", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());
    ustore_key_t const key = key_from_integer(5);
    write_text(database, blobs, {&key, 1}, "durable");

    ustore_outcome_t outcome {};
    ustore_database_flush_t flush {.database = database, .outcome = &outcome};
    ustore_database_flush(&flush);
    expect_success(outcome);
    std::error_code error;
    std::filesystem::remove_all(copied.c_str(), error);
    std::filesystem::copy(directory.c_str(), copied.c_str(), std::filesystem::copy_options::recursive, error);
    expect_true(!error);
    ustore_database_free(database);

    auto [copy, copy_status] = open_saved(config, copied.c_str());
    if (!expect_true(copy_status.succeeded())) return {};
    auto [kept, kept_status] = collection_named(receiver_t {copy}, "flushed");
    expect_true(kept_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    expect_true(arena_status.succeeded());
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(copy, kept, {&key, 1}, columns, arena, outcome);
    ustore_blobs_read(&read);
    expect_success(outcome);
    expect_true(columns.presences && (columns.presences[0] & 1) != 0 && columns.value(0) == "durable");
    ustore_database_free(copy);
    return {};
}

/** A relative directory reaches the files of its absolute spelling for the life of the handle,
 *  even after the process changes directory, on every engine that keeps its directory. */
case_result_t test_database_relative_directory(config_t const& config) {
    if (!config.directory()) return case_result_t::skipped(needs_directory_k);
    if (config.engine() == "nvme") return case_result_t::skipped("needs an engine that opens a plain directory");
    std::error_code error;
    std::filesystem::path const parent = std::filesystem::absolute(config.directory(), error);
    std::filesystem::path const absolute = parent / "relative";
    std::filesystem::path const started = std::filesystem::current_path(error);
    if (!expect_true(!error && scratch_directory_t::recreate(absolute.c_str()))) return {};
    std::filesystem::current_path(parent, error);
    auto [database, open_status] = open_saved(config, "relative");
    // The handle outlives the directory it was opened from
    std::filesystem::current_path(started, error);
    if (!expect_true(open_status.succeeded() && !error)) return {};
    auto [blobs, blobs_status] = create_collection(database, "rooted", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());
    ustore_key_t const key = key_from_integer(9);
    write_text(database, blobs, {&key, 1}, "rooted");
    ustore_outcome_t outcome {};
    ustore_database_flush_t flush {.database = database, .outcome = &outcome};
    ustore_database_flush(&flush);
    expect_success(outcome);
    ustore_database_free(database);

    auto [reopened, reopen_status] = open_saved(config, absolute.c_str());
    if (!expect_true(reopen_status.succeeded())) return {};
    auto [kept, kept_status] = collection_named(receiver_t {reopened}, "rooted");
    expect_true(kept_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    expect_true(arena_status.succeeded());
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(reopened, kept, {&key, 1}, columns, arena, outcome);
    ustore_blobs_read(&read);
    expect_success(outcome);
    expect_true(columns.presences && (columns.presences[0] & 1) != 0 && columns.value(0) == "rooted");
    ustore_database_free(reopened);
    return {};
}

/** A RAM image that exists but does not open fails the open, rather than opening an empty
 *  database over it. */
case_result_t test_database_unreadable_image(config_t const& config) {
    if (config.engine() != "ram" || !config.directory())
        return case_result_t::skipped(config.directory() ? needs_ram_engine_k : needs_directory_k);
    std::string const directory = fmt::format("{}/{}", config.directory(), "unreadable.image");
    std::string const image = fmt::format("{}/{}", directory.c_str(), "ram.image");
    expect_true(scratch_directory_t::recreate(directory.c_str()));
    // A link to itself answers ELOOP to every open, even a privileged one; its target is its own
    // name, which a link reads from its own directory, so a relative directory loops alike
    std::error_code error;
    std::filesystem::create_symlink("ram.image", image.c_str(), error);
    expect_true(!error);
    expect_status(open_saved(config, directory.c_str()), status_code_t::io_k);
    return {};
}

/** A save its callback cancels at the first report removes what it wrote, so a retry at the
 *  same path succeeds. */
case_result_t test_progress_cancel_save(config_t const& config) {
    if (!config.directory()) return case_result_t::skipped(needs_directory_k);
    auto [database, open_status] = open_database<database_t>(config);
    if (!expect_true(open_status.succeeded())) return {};
    auto [blobs, blobs_status] = create_collection(database, "progress_cancel_save.blobs", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());
    if (!database.on().supports_snapshots()) return case_result_t::skipped(needs_snapshots_k);
    ustore_key_t const key = key_from_integer(9);
    write_text(database, blobs, {&key, 1}, "saved");
    std::string const saved = fmt::format("{}/{}", config.directory(), "progress.saved");
    expect_true(ready_save_path(config, saved.c_str()));

    ustore_outcome_t outcome {};
    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t snapshot_init {.snapshot = &snapshot, .database = database, .outcome = &outcome};
    ustore_snapshot_init(&snapshot_init);
    expect_success(outcome);
    progress_record_t record {.cancelling_report = 1};
    std::string const target = save_target(config, saved);
    ustore_snapshot_save_t save {.snapshot = snapshot,
                                 .outcome = &outcome,
                                 .path = bytes_of(std::string_view {target.c_str()}),
                                 .progress = &record_progress,
                                 .progress_state = &record};
    ustore_snapshot_save(&save);
    expect_status(status_t {outcome}, status_code_t::cancelled_k);
    expect_true(record.calls == 1 && record.total != 0);
    expect_true(nothing_saved(config, saved.c_str()));

    save.progress = nullptr;
    // A RAM save failing past its open removes the partial image too, here as a directory holds
    // the image's name
    if (config.engine() == "ram") {
        std::string const blocker = fmt::format("{}/{}", saved.c_str(), "ram.image");
        expect_true(scratch_directory_t::recreate(fmt::format("{}/{}", blocker.c_str(), "kept").c_str()));
        ustore_snapshot_save(&save);
        expect_status(status_t {outcome}, status_code_t::io_k);
        std::error_code error;
        expect_true(!std::filesystem::exists(fmt::format("{}/{}", saved.c_str(), "ram.image.tmp").c_str(), error));
        std::filesystem::remove_all(blocker.c_str(), error);
    }
    ustore_snapshot_save(&save);
    expect_success(outcome);
    ustore_snapshot_free(snapshot);
    auto [copy, copy_status] = open_saved(config, saved.c_str());
    expect_true(copy_status.succeeded());
    ustore_database_free(copy);
    return {};
}

/** A compaction its callback cancels at the first report stops at that barrier, and the data
 *  stays readable. */
case_result_t test_progress_cancel_compact(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(open_status.succeeded() && arena_status.succeeded())) return {};
    auto [blobs, blobs_status] = create_collection(database, "progress.compacted", ustore_modality_blobs_k);
    expect_true(blobs_status.succeeded());
    ustore_key_t const key = key_from_integer(11);
    write_text(database, blobs, {&key, 1}, "compacted");

    ustore_outcome_t outcome {};
    progress_record_t record {.cancelling_report = 1};
    ustore_database_compact_t compact {.database = database,
                                       .outcome = &outcome,
                                       .progress = &record_progress,
                                       .progress_state = &record};
    ustore_database_compact(&compact);
    expect_status(status_t {outcome}, status_code_t::cancelled_k);
    expect_equal(record.calls, 1u);
    expect_true(record.finished <= record.total);

    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, blobs, {&key, 1}, columns, arena, outcome);
    ustore_blobs_read(&read);
    expect_success(outcome);
    expect_true(tape_written(columns.values) && columns.value(0) == "compacted");
    return {};
}

/** Finishes @p plan on @p database with @p executor, expecting @p wanted, and leaves it null, as
 *  the finish frees it whatever it answers. */
void finish_compact_plan(ustore_database_t database, ustore_executor_t executor, ustore_database_compact_plan_t& plan,
                         status_code_t wanted) {
    ustore_outcome_t outcome {};
    ustore_database_compact_plan_finish_t finish {.database = database,
                                                  .executor = executor,
                                                  .outcome = &outcome,
                                                  .plan = plan};
    ustore_database_compact_plan_finish(&finish);
    plan = nullptr;
    expect_status(outcome, wanted);
}

/** A plan a cancel leaves live continues at least one phase per call to its end, each barrier
 *  ticking that phase's own units, the phase output naming where each call stopped, and a finish
 *  frees it after its end as after a stop. Meanwhile the database admits no second compaction,
 *  and a plan without an executor, or one named with another database or executor, is refused
 *  before anything runs. */
case_result_t test_compact_plan_resumes(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [executor, spawn_status] = spawn_executor();
    auto [other_executor, other_spawn_status] = spawn_executor();
    if (!expect_true(open_status.succeeded() && spawn_status.succeeded() && other_spawn_status.succeeded())) return {};
    std::array<ustore_collection_id_t, 2> collections {};
    for (std::size_t index = 0; index != collections.size(); ++index) {
        auto [created, created_status] = create_collection(database, fmt::format("compact_plan.{}", index),
                                                           ustore_modality_blobs_k);
        if (!expect_true(created_status.succeeded())) return {};
        collections[index] = created;
        ustore_key_t const key = key_from_integer(index);
        if (!write_text(database, created, {&key, 1}, "first") || !write_text(database, created, {&key, 1}, "second"))
            return {};
    }

    ustore_outcome_t outcome {};
    ustore_database_compact_plan_t plan = nullptr;
    ustore_phase_progress_t phase {};
    progress_record_t record {.cancelling_report = 1};
    ustore_database_compact_t compact {.database = database,
                                       .executor = executor,
                                       .outcome = &outcome,
                                       .progress = &record_progress,
                                       .progress_state = &record,
                                       .plan = &plan,
                                       .phase = &phase};

    // A plan opens only with an executor to outlive it, refused before anything runs
    ustore_database_compact_t unexecuted = compact;
    unexecuted.executor = nullptr;
    ustore_database_compact(&unexecuted);
    expect_status(status_t {outcome}, status_code_t::invalid_argument_k);
    expect_true(plan == nullptr && record.calls == 0 && phase.phase == ustore_phase_none_k);

    ustore_database_compact(&compact);
    expect_status(status_t {outcome}, status_code_t::cancelled_k);
    expect_true(plan != nullptr);
    expect_true(phase.phase == ustore_phase_flush_k || phase.phase == ustore_phase_merge_k ||
                phase.phase == ustore_phase_reclaim_k);
    expect_true(phase.completed <= phase.total);
    expect_equal(record.calls, 1u);
    if (!expect_true(record.finished <= record.total)) return {};

    // The live plan holds the database, so neither a second plan nor a call naming none opens
    ustore_database_compact_plan_t second = nullptr;
    ustore_database_compact_t contending {.database = database,
                                          .executor = executor,
                                          .outcome = &outcome,
                                          .plan = &second};
    ustore_database_compact(&contending);
    expect_status(status_t {outcome}, status_code_t::contended_k);
    expect_true(second == nullptr);
    ustore_database_compact_t one_shot {.database = database, .outcome = &outcome, .phase = &phase};
    ustore_database_compact(&one_shot);
    expect_status(status_t {outcome}, status_code_t::contended_k);
    expect_true(phase.phase == ustore_phase_none_k);

    // A plan names the database and the executor it was opened with, checked before the database
    ustore_database_t other = nullptr;
    ustore_database_init_ram_t init_other {.database = &other, .outcome = &outcome};
    ustore_database_init_ram(&init_other);
    expect_success(outcome);
    ustore_database_compact_t elsewhere = compact;
    elsewhere.database = other;
    ustore_database_compact(&elsewhere);
    expect_status(status_t {outcome}, status_code_t::invalid_argument_k);
    elsewhere.database = database;
    elsewhere.executor = other_executor;
    ustore_database_compact(&elsewhere);
    expect_status(status_t {outcome}, status_code_t::invalid_argument_k);
    elsewhere.database = nullptr;
    ustore_database_compact(&elsewhere);
    expect_status(status_t {outcome}, status_code_t::uninitialized_k);

    // Each call runs at least one more phase, and a cancel at the closing barrier succeeds
    ustore_size_t calls = 1;
    for (; calls != 4096 && status_t {outcome}.code != status_code_t::success_k; ++calls) {
        record.calls = 0;
        ustore_database_compact(&compact);
        expect_true(status_t {outcome}.code == status_code_t::cancelled_k ||
                    status_t {outcome}.code == status_code_t::success_k);
        expect_equal(record.calls, 1u);
        expect_true(record.finished <= record.total);
    }
    expect_success(outcome);
    expect_true(calls > 1);
    expect_equal(record.finished, record.total);
    expect_true(phase.phase == ustore_phase_done_k && phase.ordinal != 0);
    record.calls = 0;
    ustore_database_compact(&compact);
    expect_success(outcome);
    expect_true(record.calls == 0 && phase.phase == ustore_phase_done_k);
    finish_compact_plan(database, executor, plan, status_code_t::success_k);

    // Once finished, the database admits a call naming no plan, which runs to its end
    ustore_database_compact(&one_shot);
    expect_success(outcome);
    expect_true(phase.phase == ustore_phase_done_k);

    // A plan finished after a stop frees the database too, as does one finished naming another
    record = {.cancelling_report = 1};
    ustore_database_compact(&compact);
    expect_status(status_t {outcome}, status_code_t::cancelled_k);
    finish_compact_plan(database, executor, plan, status_code_t::success_k);
    record = {.cancelling_report = 1};
    ustore_database_compact(&compact);
    expect_status(status_t {outcome}, status_code_t::cancelled_k);
    finish_compact_plan(other, executor, plan, status_code_t::invalid_argument_k);
    record = {.cancelling_report = 1};
    ustore_database_compact(&compact);
    expect_status(status_t {outcome}, status_code_t::cancelled_k);
    finish_compact_plan(database, other_executor, plan, status_code_t::invalid_argument_k);
    finish_compact_plan(database, nullptr, plan, status_code_t::invalid_argument_k);
    finish_compact_plan(nullptr, nullptr, plan, status_code_t::uninitialized_k);
    ustore_database_compact(&one_shot);
    expect_success(outcome);

    // A deferred plan stops, continues to its end and finishes on its executor too
    record = {.outcome = &outcome, .cancelling_report = 1};
    ustore_database_compact_t deferred = compact;
    deferred.completion = ustore_completion_deferred_k;
    ustore_database_compact(&deferred);
    expect_true(ustore_outcome_wait(&outcome) == ustore_status_cancelled_k);
    expect_true(plan != nullptr && record.after_final == 0);
    record.cancelling_report = 0;
    ustore_database_compact(&deferred);
    expect_true(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    expect_true(phase.phase == ustore_phase_done_k && record.finished == record.total);
    ustore_database_compact_plan_finish_t finish {.database = database,
                                                  .executor = executor,
                                                  .outcome = &outcome,
                                                  .completion = ustore_completion_deferred_k,
                                                  .plan = plan};
    ustore_database_compact_plan_finish(&finish);
    expect_true(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    plan = nullptr;

    for (std::size_t index = 0; index != collections.size(); ++index) {
        ustore_key_t const key = key_from_integer(index);
        auto [arena, arena_status] = arena_t::make();
        expect_true(arena_status.succeeded());
        read_columns_t columns;
        ustore_blobs_read_t read = read_of(database, collections[index], {&key, 1}, columns, arena, outcome);
        ustore_blobs_read(&read);
        expect_success(outcome);
        expect_true(tape_written(columns.values) && columns.value(0) == "second");
    }
    ustore_database_free(other);
    ustore_executor_free(other_executor);
    ustore_executor_free(executor);
    return {};
}

/** Every phase a plan runs spells its name and reads it back, while a name no phase holds is
 *  refused. */
void test_compact_phase_names(config_t const&) {
    expect_spellings(ustore_phase_name, ustore_phase_from_name, 5);
    expect_true(std::string_view {ustore_phase_name(ustore_phase_merge_k)} == "merge");
    expect_true(std::string_view {ustore_phase_name(static_cast<ustore_phase_t>(5))} == "unrecognized");
}

/** The callbacks of a deferred scan, pull, compaction and save each run one at a time, before their
 *  call is final, with counts that end at the total and never decrease, except where a phase of a
 *  compaction ends. The record counts without atomics, so ThreadSanitizer flags two callbacks of
 *  one call running at once. */
void test_progress_serialized(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [executor, spawn_status] = spawn_executor();
    if (!expect_true(open_status.succeeded() && spawn_status.succeeded())) return;
    auto [arena, arena_status] = arena_t::make(executor);
    bool const arena_made = expect_true(arena_status.succeeded());
    auto [blobs, blobs_status] = create_collection(database, "progress_serialized.blobs", ustore_modality_blobs_k);
    if (!expect_true(blobs_status.succeeded()) || !arena_made) return;
    std::array<ustore_key_t, 4> const keys {key_from_integer(1), key_from_integer(2), key_from_integer(3),
                                            key_from_integer(4)};
    if (!write_text(database, blobs, keys, "reported")) return;

    // Every report must come back clean, and a wait between the calls checks none comes late
    auto const expect_clean = [&](progress_record_t const& record, ustore_size_t calls) {
        expect_equal(record.calls, calls);
        expect_true(record.overlaps == 0 && record.decreases == 0 && record.after_final == 0);
        expect_equal(record.finished, record.total);
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
    expect_true(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    expect_clean(scanned, 64);

    progress_record_t pulled_record {.outcome = &outcome};
    auto [cursor, cursor_status] = open_cursor(database, blobs);
    expect_true(cursor_status.succeeded());
    pulled_keys_t pulled;
    ustore_blobs_cursor_next_t next = pull_of(database, cursor, pulled, arena, 2, outcome);
    next.executor = executor;
    next.completion = ustore_completion_deferred_k;
    next.progress = &record_progress;
    next.progress_state = &pulled_record;
    ustore_blobs_cursor_next(&next);
    expect_true(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    expect_clean(pulled_record, 1);
    ustore_blobs_cursor_free(cursor);

    progress_record_t compacted {.outcome = &outcome};
    ustore_database_compact_t compact {.database = database,
                                       .executor = executor,
                                       .outcome = &outcome,
                                       .completion = ustore_completion_deferred_k,
                                       .progress = &record_progress,
                                       .progress_state = &compacted};
    ustore_database_compact(&compact);
    expect_true(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    expect_true(compacted.calls != 0);
    // Each barrier ticks its own phase's units, so the count drops where a phase ends
    expect_true(compacted.overlaps == 0 && compacted.after_final == 0);
    expect_equal(compacted.finished, compacted.total);

    if (config.directory() && database.on().supports_snapshots()) {
        std::string const saved = fmt::format("{}/{}", config.directory(), "progress.serialized");
        expect_true(ready_save_path(config, saved.c_str()));
        ustore_snapshot_t snapshot = nullptr;
        ustore_snapshot_init_t snapshot_init {.snapshot = &snapshot, .database = database, .outcome = &outcome};
        ustore_snapshot_init(&snapshot_init);
        expect_success(outcome);
        progress_record_t saved_record {.outcome = &outcome};
        std::string const target = save_target(config, saved);
        ustore_snapshot_save_t save {.snapshot = snapshot,
                                     .executor = executor,
                                     .outcome = &outcome,
                                     .completion = ustore_completion_deferred_k,
                                     .path = bytes_of(std::string_view {target.c_str()}),
                                     .progress = &record_progress,
                                     .progress_state = &saved_record};
        ustore_snapshot_save(&save);
        expect_true(ustore_outcome_wait(&outcome) == ustore_status_success_k);
        expect_true(saved_record.calls != 0 && saved_record.total != 0);
        expect_clean(saved_record, saved_record.calls);
        ustore_snapshot_free(snapshot);
    }

    std::size_t const calls = scanned.calls + compacted.calls;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    expect_equal(scanned.calls + compacted.calls, calls);
    ustore_executor_free(executor);
}

#pragma endregion Progress

/** A save copies exactly the collections and names its snapshot recorded: one created after the
 *  snapshot stays behind, and one dropped after it comes along with its pairs. */
case_result_t test_snapshot_save_recorded(config_t const& config) {
    if (!config.directory()) return case_result_t::skipped(needs_directory_k);
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(open_status.succeeded() && arena_status.succeeded())) return {};
    if (!database.on().supports_snapshots()) return case_result_t::skipped(needs_snapshots_k);
    auto [dropped, dropped_status] = create_collection(database, "save.dropped", ustore_modality_blobs_k);
    if (!expect_true(dropped_status.succeeded())) return {};
    ustore_key_t const key = key_from_integer(21);
    if (!write_text(database, dropped, {&key, 1}, "recorded")) return {};

    ustore_outcome_t outcome {};
    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t snapshot_init {.snapshot = &snapshot, .database = database, .outcome = &outcome};
    ustore_snapshot_init(&snapshot_init);
    expect_success(outcome);
    expect_true(create_collection(database, "save.created", ustore_modality_blobs_k).has_value());
    ustore_collection_drop_t drop {.database = database, .outcome = &outcome, .collection = dropped};
    ustore_collection_drop(&drop);
    bool const refuses_drop = config.engine() == "ram" || config.engine() == "rocksdb";
    refuses_drop ? expect_status(outcome, status_code_t::invalid_argument_k) : expect_success(outcome);

    std::string const saved = fmt::format("{}/{}", config.directory(), "save.recorded");
    expect_true(ready_save_path(config, saved.c_str()));
    std::string const target = save_target(config, saved);
    ustore_snapshot_save_t save {.snapshot = snapshot,
                                 .outcome = &outcome,
                                 .path = bytes_of(std::string_view {target.c_str()})};
    ustore_snapshot_save(&save);
    expect_success(outcome);
    ustore_snapshot_free(snapshot);

    auto [copy, copy_status] = open_saved(config, saved.c_str());
    if (!expect_true(copy_status.succeeded())) return {};
    expect_status(collection_named(receiver_t {copy}, "save.created"), status_code_t::not_found_k);
    auto [kept, kept_status] = collection_named(receiver_t {copy}, "save.dropped");
    expect_true(kept_status.succeeded() && kept == dropped);
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(copy, dropped, {&key, 1}, columns, arena, outcome);
    ustore_blobs_read(&read);
    expect_success(outcome);
    expect_true(tape_written(columns.values) && columns.value(0) == "recorded");
    ustore_database_free(copy);
    return {};
}

#pragma region Change Feeds

/** One change a test committed, as a feed must hand it back: a write holds its round. */
struct logged_change_t {
    ustore_stamp_t stamp = 0;
    ustore_key_t key {};
    ustore_change_kind_t kind = ustore_change_write_k;
    std::size_t round = 0;
};

/** An open feed and how many logged changes it handed out, after those at or below its stamp. */
struct open_feed_t {
    ustore_changes_cursor_t cursor = nullptr;
    ustore_stamp_t since_stamp = 0;
    std::size_t handed = 0;
};

/** Opens a feed of @p collection after @p since_stamp, answering the outcome's status. */
status_t open_feed(ustore_database_t database, ustore_collection_id_t const& collection, ustore_stamp_t since_stamp,
                   ustore_changes_cursor_t& cursor) noexcept {
    ustore_outcome_t outcome {};
    ustore_changes_cursor_init_t init {
        .database = database,
        .outcome = &outcome,
        .collection = collection,
        .since_stamp = since_stamp,
        .cursor = &cursor,
    };
    ustore_changes_cursor_init(&init);
    return status_t {outcome};
}

/** Pulls up to @p wanted changes of @p feed with their values, checking each against the logged
 *  changes after its stamp in @p log, and advances it past them. */
void pull_feed(open_feed_t& feed, std::span<logged_change_t const> log, std::size_t wanted, arena_t& arena) {
    std::size_t first = 0;
    while (first != log.size() && log[first].stamp <= feed.since_stamp) ++first;
    std::size_t const remaining = log.size() - first - feed.handed;
    ustore_outcome_t outcome {};
    ustore_size_t count = 0;
    ustore_key_t const* keys = nullptr;
    ustore_stamp_t const* stamps = nullptr;
    ustore_change_kind_t const* kinds = nullptr;
    ustore_strings_view_t values {};
    ustore_changes_cursor_next_t next {
        .cursor = feed.cursor,
        .outcome = &outcome,
        .wanted = wanted,
        .count = &count,
        .keys = &keys,
        .stamps = &stamps,
        .kinds = &kinds,
        .values = &values,
        .arena = arena,
    };
    ustore_changes_cursor_next(&next);
    expect_success(outcome);
    if (!expect_equal(std::size_t {count}, remaining < wanted ? remaining : wanted)) return;
    for (std::size_t index = 0; index != count; ++index) {
        logged_change_t const& logged = log[first + feed.handed + index];
        expect_true(keys[index] == logged.key && stamps[index] == logged.stamp && kinds[index] == logged.kind);
        std::string const value = logged.kind == ustore_change_write_k ? fmt::format("v{}", logged.round) : "";
        expect_true(string_of(tape_string(values, index)) == value);
    }
    feed.handed += count;
}

/**
 *  @brief A randomized history of commits to one collection beside another, range removals at the
 *      head and compactions between them, read back through feeds opened at random stamps against
 *      the log of what was committed, a range removal as the removal of every key it covered.
 *
 *  A feed is refused only below every stamp an open feed pins and below the newest commit, and,
 *  on the engines that keep their own versions, a feed below what a compaction with no pins
 *  dropped is refused.
 */
case_result_t test_changes_randomized_history(config_t const& config) {
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    if (!expect_true(open_status.succeeded() && arena_status.succeeded())) return {};
    auto [watched, watched_status] = create_collection(database, "changes_history.watched", ustore_modality_blobs_k);
    auto [beside, beside_status] = create_collection(database, "changes_history.beside", ustore_modality_blobs_k);
    if (!expect_true(watched_status.succeeded() && beside_status.succeeded())) return {};
    if (!database.on().supports_transactions()) return case_result_t::skipped(needs_transactions_k);

    ustore_outcome_t outcome {};
    ustore_snapshot_t snapshot = nullptr;
    ustore_stamp_t head = 0;
    ustore_snapshot_init_t pin {.snapshot = &snapshot, .database = database, .outcome = &outcome, .read_stamp = &head};
    ustore_snapshot_init(&pin);
    expect_success(outcome);
    ustore_snapshot_free(snapshot);
    ustore_changes_cursor_t probe = nullptr;
    if (open_feed(database, watched, head, probe) == status_code_t::unsupported_k)
        return case_result_t::skipped("needs change stamps");
    ustore_changes_cursor_free(probe);

    constexpr std::size_t keys_count_k = 16, rounds_k = 160, feeds_max_k = 6;
    std::array<bool, keys_count_k> present {};
    st::vector<logged_change_t> log;
    st::vector<ustore_stamp_t> stamps;
    expect_true(stamps.push_back(ustore_stamp_t {head}) == st::status_t::success_k);
    std::array<open_feed_t, feeds_max_k> feeds {};
    split_mix_generator_t generator {case_stream_key()};
    auto draw = [&](std::size_t below) noexcept { return static_cast<std::size_t>(generator.below(below)); };

    // The history stops at its first failed check, after which the log misdescribes the collection
    std::size_t const failed_before = test_run.failed_checks.load();
    for (std::size_t round = 1; round <= rounds_k && test_run.failed_checks.load() == failed_before; ++round) {
        std::size_t const action = draw(22);
        if (action < 10) {
            expected<transaction_t> maybe_transaction = database.on().transaction();
            if (!expect_true(maybe_transaction.has_value())) break;
            ustore_transaction_t const transaction = maybe_transaction->transaction();
            std::size_t const first_key = draw(keys_count_k), touched = 1 + draw(4);
            std::array<logged_change_t, 4> staged {};
            for (std::size_t offset = 0; offset != touched; ++offset) {
                std::size_t const index = (first_key + offset) % keys_count_k;
                ustore_key_t const key = key_from_integer(index);
                bool const removing = present[index] && draw(3) == 0;
                if (removing) {
                    ustore_blobs_remove_t remove {.database = database,
                                                  .outcome = &outcome,
                                                  .transaction = transaction,
                                                  .count = 1,
                                                  .collections = {&watched, 0},
                                                  .keys = {&key, 0}};
                    ustore_blobs_remove(&remove);
                    expect_success(outcome);
                }
                else write_text(database, watched, {&key, 1}, fmt::format("v{}", round), transaction);
                write_text(database, beside, {&key, 1}, "beside", transaction);
                staged[offset] = {0, key, removing ? ustore_change_removal_k : ustore_change_write_k, round};
                present[index] = !removing;
            }
            ustore_stamp_t commit_stamp = 0;
            ustore_transaction_commit_t commit {.database = database,
                                                .outcome = &outcome,
                                                .transaction = transaction,
                                                .commit_stamp = &commit_stamp};
            ustore_transaction_commit(&commit);
            expect_success(outcome);
            std::sort(staged.begin(), staged.begin() + touched,
                      [](logged_change_t const& first, logged_change_t const& second) noexcept {
                          return first.key < second.key;
                      });
            for (std::size_t offset = 0; offset != touched; ++offset) {
                staged[offset].stamp = commit_stamp;
                expect_true(log.push_back(logged_change_t {staged[offset]}) == st::status_t::success_k);
            }
            expect_true(stamps.push_back(ustore_stamp_t {commit_stamp}) == st::status_t::success_k);
            head = commit_stamp;
        }
        else if (action < 12) {
            // A range removal at the head comes back as the removal of every key it covered
            std::size_t const lower = draw(keys_count_k), upper = lower + 1 + draw(keys_count_k - lower);
            ustore_key_t const start = key_from_integer(lower), end = key_from_integer(upper);
            ustore_blobs_remove_t remove {.database = database,
                                          .outcome = &outcome,
                                          .count = 1,
                                          .collections = {&watched, 0},
                                          .keys = {&start, 0},
                                          .end_keys = {&end, 0}};
            ustore_blobs_remove(&remove);
            expect_success(outcome);
            ustore_snapshot_init(&pin);
            expect_success(outcome);
            ustore_snapshot_free(snapshot);
            for (std::size_t index = lower; index != upper; ++index) {
                if (!present[index]) continue;
                present[index] = false;
                expect_true(log.push_back(logged_change_t {head, key_from_integer(index), ustore_change_removal_k,
                                                           round}) == st::status_t::success_k);
            }
            expect_true(stamps.push_back(ustore_stamp_t {head}) == st::status_t::success_k);
        }
        else if (action < 14) {
            ustore_database_compact_t compact {.database = database, .outcome = &outcome};
            ustore_database_compact(&compact);
            expect_success(outcome);
        }
        else if (action < 17) {
            open_feed_t* const slot = &feeds[draw(feeds_max_k)];
            if (slot->cursor) continue;
            ustore_stamp_t const since = draw(2) ? head : stamps[draw(stamps.size())];
            status_t const opened = open_feed(database, watched, since, slot->cursor);
            bool pinned_below = since == head;
            for (open_feed_t const& feed : feeds)
                pinned_below = pinned_below ||
                               (feed.cursor && feed.cursor != slot->cursor && feed.since_stamp <= since);
            if (opened.failed()) {
                expect_true(opened == status_code_t::write_conflict_k && !pinned_below);
                slot->cursor = nullptr;
                continue;
            }
            slot->since_stamp = since;
            slot->handed = 0;
        }
        else if (action < 21) {
            open_feed_t& feed = feeds[draw(feeds_max_k)];
            if (feed.cursor) pull_feed(feed, log, 1 + draw(6), arena);
        }
        else {
            open_feed_t& feed = feeds[draw(feeds_max_k)];
            ustore_changes_cursor_free(feed.cursor);
            feed = {};
        }
    }
    for (open_feed_t& feed : feeds) {
        if (feed.cursor) pull_feed(feed, log, log.size() + 1, arena);
        ustore_changes_cursor_free(feed.cursor);
        feed = {};
    }

    // Two overwrites and a compaction with no feed open drop the versions between them
    if (config.engine() == "ram" || config.engine() == "nvme") {
        ustore_key_t const key = key_from_integer(0);
        write_text(database, watched, {&key, 1}, "first");
        write_text(database, watched, {&key, 1}, "second");
        ustore_database_compact_t compact {.database = database, .outcome = &outcome};
        ustore_database_compact(&compact);
        expect_success(outcome);
        ustore_changes_cursor_t dropped = nullptr;
        expect_status(open_feed(database, watched, stamps[0], dropped), status_code_t::write_conflict_k);
        expect_true(dropped == nullptr);
    }
    return {};
}

#pragma endregion Change Feeds

int main(int argc, char** argv) {
    start_test_run(argc, argv);

    auto const maybe_directory = scratch_directory_t::make(test_run.settings.directory);
    if (!maybe_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", test_run.settings.directory);
        return 1;
    }
    config_t const config {test_run.settings.engine, maybe_directory->path()};

    run_test_case(engine_label_t {"test_status_names"}.c_str(), test_status_names, config);
    run_test_case(engine_label_t {"test_closed_set_names"}.c_str(), test_closed_set_names, config);
    run_test_case(engine_label_t {"test_database_init_refusals"}.c_str(), test_database_init_refusals, config);
    run_test_case(engine_label_t {"test_database_validation"}.c_str(), test_database_validation, config);
    run_test_case(engine_label_t {"test_database_forged_views"}.c_str(), test_database_forged_views, config);
    run_test_case(engine_label_t {"test_database_foreign_handles"}.c_str(), test_database_foreign_handles, config);
    run_test_case(engine_label_t {"test_database_failed_reports_zeroed"}.c_str(), test_database_failed_reports_zeroed,
                  config);
    run_test_case(engine_label_t {"test_database_cpu_class_ceiling"}.c_str(), test_database_cpu_class_ceiling, config);
    run_test_case(engine_label_t {"test_database_transaction_reset_elsewhere"}.c_str(),
                  test_database_transaction_reset_elsewhere, config);
    run_test_case(engine_label_t {"test_database_read_offsets"}.c_str(), test_database_read_offsets, config);
    run_test_case(engine_label_t {"test_database_write_layouts"}.c_str(), test_database_write_layouts, config);
    run_test_case(engine_label_t {"test_database_write_broadcast"}.c_str(), test_database_write_broadcast, config);
    run_test_case(engine_label_t {"test_database_unknown_layout"}.c_str(), test_database_unknown_layout, config);
    run_test_case(engine_label_t {"test_database_lengths_without_bytes"}.c_str(), test_database_lengths_without_bytes,
                  config);
    run_test_case(engine_label_t {"test_database_tape_without_bytes"}.c_str(), test_database_tape_without_bytes,
                  config);
    run_test_case(engine_label_t {"test_database_zeroed_fields"}.c_str(), test_database_zeroed_fields, config);
    run_test_case(engine_label_t {"test_database_modality_refusals"}.c_str(), test_database_modality_refusals, config);
    run_test_case(engine_label_t {"test_database_modality_keys"}.c_str(), test_database_modality_keys, config);
    run_test_case(engine_label_t {"test_database_strings_layouts"}.c_str(), test_database_strings_layouts, config);
    run_test_case(engine_label_t {"test_database_measure_whole"}.c_str(), test_database_measure_whole, config);
    run_test_case(engine_label_t {"test_database_remove"}.c_str(), test_database_remove, config);
    run_test_case(engine_label_t {"test_database_rollback"}.c_str(), test_database_rollback, config);
    run_test_case(engine_label_t {"test_database_stamps"}.c_str(), test_database_stamps, config);
    run_test_case(engine_label_t {"test_database_free_holds"}.c_str(), test_database_free_holds, config);
    run_test_case(engine_label_t {"test_snapshot_reads_collection_dropped_after_open"}.c_str(),
                  test_snapshot_reads_collection_dropped_after_open, config);
    run_test_case(engine_label_t {"test_database_find_at_snapshot"}.c_str(), test_database_find_at_snapshot, config);
    run_test_case(engine_label_t {"test_database_fresh_collections"}.c_str(), test_database_fresh_collections, config);
    run_test_case(engine_label_t {"test_database_describe_compact"}.c_str(), test_database_describe_compact, config);
    run_test_case(engine_label_t {"test_database_flush_durable"}.c_str(), test_database_flush_durable, config);
    run_test_case(engine_label_t {"test_database_clear_modes"}.c_str(), test_database_clear_modes, config);
    run_test_case(engine_label_t {"test_database_view_refusals"}.c_str(), test_database_view_refusals, config);
    run_test_case(engine_label_t {"test_database_view_nvme"}.c_str(), test_database_view_nvme, config);
    run_test_case(engine_label_t {"test_database_open_budgets"}.c_str(), test_database_open_budgets, config);
    run_test_case(engine_label_t {"test_progress_cancel_scans"}.c_str(), test_progress_cancel_scans, config);
    run_test_case(engine_label_t {"test_progress_cancel_cursor"}.c_str(), test_progress_cancel_cursor, config);
    run_test_case(engine_label_t {"test_database_relative_directory"}.c_str(), test_database_relative_directory,
                  config);
    run_test_case(engine_label_t {"test_database_unreadable_image"}.c_str(), test_database_unreadable_image, config);
    run_test_case(engine_label_t {"test_progress_cancel_save"}.c_str(), test_progress_cancel_save, config);
    run_test_case(engine_label_t {"test_progress_cancel_compact"}.c_str(), test_progress_cancel_compact, config);
    run_test_case(engine_label_t {"test_compact_plan_resumes"}.c_str(), test_compact_plan_resumes, config);
    run_test_case(engine_label_t {"test_compact_phase_names"}.c_str(), test_compact_phase_names, config);
    run_test_case(engine_label_t {"test_progress_serialized"}.c_str(), test_progress_serialized, config);
    run_test_case(engine_label_t {"test_snapshot_save_recorded"}.c_str(), test_snapshot_save_recorded, config);
    run_test_case(engine_label_t {"test_changes_randomized_history"}.c_str(), test_changes_randomized_history, config);
    return finish_test_run();
}
