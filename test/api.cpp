/**
 *  @file test/api.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Checks that the C ABI accepts valid argument combinations and rejects malformed ones.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`, `std::uintptr_t`
#include <cstdio>  // `stderr`

#include <array>       // `std::array`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`

#include <fmt/format.h>

#include "ustore/ustore.hpp"

#include "test.hpp" // `suite_exit_code`, `expect`, `expect_failure`, `expect_status`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum;

/** Every engine refuses an init without an output, and all but the in-memory one refuse to open without a directory. */
std::size_t database_init_refusals(test_config_t const& config) {
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

/** Writes, reads, transactions and scans accept valid tasks and reject malformed ones, per engine capability. */
std::size_t database_validation(test_config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    blobs_collection_t collection = database.main();
    auto maybe_named_collection = database.find_or_create("col");
    failures += expect(static_cast<bool>(maybe_named_collection) == database.supports_collections());
    auto maybe_transaction = database.transact();
    transaction_t transaction = maybe_transaction.has_value() ? *std::move(maybe_transaction) : transaction_t {};

    constexpr ustore_length_t count = 3;
    std::array<ustore_key_t, count> keys {key_from_integer(34), key_from_integer(35), key_from_integer(36)};
    std::array<std::uint64_t, count> numbers {34, 35, 36};
    ustore_length_t number_length = sizeof(std::uint64_t);
    std::array<ustore_length_t, count> number_starts {0, number_length, number_length * 2};
    auto numbers_begin = reinterpret_cast<ustore_bytes_ptr_t>(numbers.data());

    contents_arg_t values {
        .starts_begin = {number_starts.data(), sizeof(ustore_length_t)},
        .lengths_begin = {&number_length, 0},
        .contents_begin = {&numbers_begin, 0},
        .count = count,
    };

    using value_extractor_t = contents_arg_extractor<contents_arg_t>;
    strided_iterator<ustore_bytes_cptr_t const> const contents = value_extractor_t {}.contents(values);
    strided_iterator<ustore_length_t const> const starts = value_extractor_t {}.starts(values);
    strided_iterator<ustore_length_t const> const lengths = value_extractor_t {}.lengths(values);

    ustore_outcome_t outcome {};

#pragma region Writes

    // Every write below varies one field of this valid one
    ustore_write_t const write_head {
        .database = database,
        .outcome = &outcome,
        .count = count,
        .collections = collection.member_ptr(),
        .keys = keys.data(),
        .keys_stride = sizeof(ustore_key_t),
        .starts = starts.get(),
        .starts_stride = starts.stride(),
        .lengths = lengths.get(),
        .lengths_stride = lengths.stride(),
        .values = contents.get(),
        .values_stride = contents.stride(),
    };
    for (ustore_durability_t const durability :
         std::array {ustore_durability_buffered_k, ustore_durability_flushed_k}) {
        ustore_write_t write = write_head;
        write.durability = durability;
        ustore_write(&write);
        failures += expect_success(outcome);
    }

    if (!database.supports_collections()) {
        std::array<ustore_collection_id_t, count> collections {
            {{uuid_from_integer(1)}, {uuid_from_integer(2)}, {uuid_from_integer(3)}}};
        ustore_write_t write_unnamed = write_head;
        write_unnamed.collections = collections.data();
        write_unnamed.collections_stride = sizeof(ustore_collection_id_t);
        ustore_write(&write_unnamed);
        failures += expect_failure(outcome);

        std::array<ustore_collection_id_t, count> collections_only_main {
            ustore_collection_main_k, ustore_collection_main_k, ustore_collection_main_k};
        ustore_write_t write_main = write_head;
        write_main.collections = collections_only_main.data();
        write_main.collections_stride = sizeof(ustore_collection_id_t);
        ustore_write(&write_main);
        failures += expect_success(outcome);
    }

    ustore_write_t write_null_collection = write_head;
    write_null_collection.collections = nullptr;
    ustore_write(&write_null_collection);
    failures += expect_success(outcome);

    if (maybe_named_collection) {
        ustore_write_t write_named = write_head;
        write_named.collections = maybe_named_collection->member_ptr();
        ustore_write(&write_named);
        failures += expect_success(outcome);
    }

    if (database.supports_transactions()) {
        ustore_write_t write = write_head;
        write.transaction = transaction;
        ustore_write(&write);
        failures += expect_success(outcome);

        // A transaction flushes at commit, not at a write
        write.durability = ustore_durability_flushed_k;
        ustore_write(&write);
        failures += expect_failure(outcome);
    }

    // No tasks, yet keys are given
    ustore_write_t write_no_tasks = write_head;
    write_no_tasks.count = 0;
    write_no_tasks.collections_stride = 0;
    ustore_write(&write_no_tasks);
    failures += expect_failure(outcome);

    // Tasks without keys
    ustore_write_t write_null_keys = write_head;
    write_null_keys.keys = nullptr;
    ustore_write(&write_null_keys);
    failures += expect_failure(outcome);

    // Only a transaction records the keys it writes
    ustore_write_t write_blind_head = write_head;
    write_blind_head.watch = ustore_watch_none_k;
    ustore_write(&write_blind_head);
    failures += expect_failure(outcome);

#pragma endregion Writes

#pragma region Reads

    std::array<ustore_length_t, count + 1> found_offsets {};
    ustore_byte_t* found_values = nullptr;
    ustore_read_t read {
        .database = database,
        .outcome = &outcome,
        .count = count,
        .collections = collection.member_ptr(),
        .keys = keys.data(),
        .keys_stride = sizeof(ustore_key_t),
        .offsets = found_offsets.data(),
        .values = &found_values,
        .arena = collection.member_arena(),
    };
    ustore_read(&read);
    failures += expect_success(outcome);

    if (database.supports_transactions()) {
        read.view = ustore_transaction_view(transaction, ustore_watch_none_k);
        ustore_read(&read);
        failures += expect_success(outcome);
    }

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

    ustore_length_t const limit = count;
    std::array<ustore_length_t, 2> scan_offsets {};
    std::array<ustore_key_t, count> found_keys {};
    ustore_scan_t scan {
        .database = database,
        .outcome = &outcome,
        .view = ustore_transaction_view(transaction, ustore_watch_keys_k),
        .count = 1,
        .collections = collection.member_ptr(),
        .start_keys = keys.data(),
        .count_limits = &limit,
        .offsets = scan_offsets.data(),
        .keys = found_keys.data(),
    };
    ustore_scan(&scan);
    failures += expect_success(outcome);

    ustore_scan_t scan_without_start_keys = scan;
    scan_without_start_keys.start_keys = nullptr;
    ustore_scan(&scan_without_start_keys);
    failures += expect_failure(outcome);

    ustore_scan_t scan_without_limits = scan;
    scan_without_limits.count_limits = nullptr;
    ustore_scan(&scan_without_limits);
    failures += expect_failure(outcome);

#pragma endregion Scans

    return failures;
}

/** A pointer neither view constructor made, a raw handle with a stray bit above the tag or a bare transaction, is
 *  refused before any engine reads through it. */
std::size_t database_forged_views(test_config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;

    ustore_key_t const key = key_from_integer(1);
    ustore_outcome_t outcome {};
    ustore_read_t read {};
    read.database = database;
    read.outcome = &outcome;
    read.count = 1;
    read.keys = &key;

    // Handles align past the two tag bits, so the next bit up leaves the tag empty while the pointer stays set
    std::uintptr_t const handle_bits = reinterpret_cast<std::uintptr_t>(static_cast<ustore_database_t>(database));
    read.view = reinterpret_cast<ustore_view_t>(handle_bits | 4u);
    ustore_read(&read);
    failures += expect_status(outcome, status_code_t::invalid_argument_k);

    expected<transaction_t> maybe_transaction = database.transact();
    if (!maybe_transaction) return failures;
    read.view = reinterpret_cast<ustore_view_t>(maybe_transaction->transaction());
    ustore_read(&read);
    failures += expect_status(outcome, status_code_t::invalid_argument_k);
    return failures;
}

/** A view or a transaction of another engine is refused before either engine sees it. */
case_result_t database_foreign_handles(test_config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;

    // The in-memory engine needs no directory, so it is the other engine beside every disk engine
    database_t other;
    if (config.engine() != "ram") {
        engine_options_t ram = config.options();
        ram.name = "ram";
        auto [opened, other_status] = open_database<database_t>(test_config_t {ram, nullptr});
        failures += expect(other_status.succeeded());
        other = std::move(opened);
    }
    else if (config.directory()) {
        test_path_t const directory {config.directory(), "other"};
        failures += expect(recreate_directory(directory.c_str()));
        for (std::string_view const name : std::array<std::string_view, 2> {"rocksdb", "leveldb"}) {
            engine_options_t disk = config.options();
            disk.name = name;
            if (auto opened = open_database<database_t>(test_config_t {disk, directory.c_str()}); opened) {
                other = *std::move(opened);
                break;
            }
        }
    }
    else return case_result_t::skipped(needs_directory_k, failures);
    if (!other) return case_result_t::skipped("only one engine is built", failures);

    expected<transaction_t> maybe_transaction = other.transact();
    expected<snapshot_t> maybe_snapshot = other.snapshot();
    // A transaction is tried first, since it is refused both as a view and as the transaction of a write
    ustore_view_t foreign = nullptr;
    if (maybe_transaction) foreign = ustore_transaction_view(maybe_transaction->transaction(), ustore_watch_keys_k);
    else if (maybe_snapshot) foreign = ustore_snapshot_view(maybe_snapshot->snapshot());
    else return case_result_t::skipped("the other engine hands out no views", failures);

    ustore_key_t const key = key_from_integer(1);
    ustore_outcome_t outcome {};
    ustore_read_t read {};
    read.database = database;
    read.outcome = &outcome;
    read.view = foreign;
    read.count = 1;
    read.keys = &key;
    ustore_read(&read);
    failures += expect_status(outcome, status_code_t::invalid_argument_k);
    if (!maybe_transaction) return failures;

    ustore_bytes_cptr_t const value = nullptr;
    ustore_length_t const length = 0;
    ustore_write_t write {};
    write.database = database;
    write.outcome = &outcome;
    write.transaction = maybe_transaction->transaction();
    write.count = 1;
    write.keys = &key;
    write.lengths = &length;
    write.values = &value;
    ustore_write(&write);
    failures += expect_status(outcome, status_code_t::invalid_argument_k);
    return failures;
}

/** A call given an executor answers unsupported, since this build runs every call on the calling thread. */
std::size_t database_executor_refusal(test_config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;

    // The executor is refused before anything reads through it, so any address stands in for one
    int stand_in = 0;
    ustore_key_t const key = key_from_integer(1);
    ustore_outcome_t outcome {};
    ustore_read_t read {};
    read.database = database;
    read.executor = reinterpret_cast<ustore_executor_t>(&stand_in);
    read.outcome = &outcome;
    read.count = 1;
    read.keys = &key;
    ustore_read(&read);
    failures += expect_status(outcome, status_code_t::unsupported_k);
    return failures;
}

/** A read exports @c count + 1 offsets, the last closing the final value, and nothing past them. */
std::size_t database_read_offsets(test_config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;

    constexpr std::size_t count_k = 3;
    std::array<ustore_key_t, count_k> const keys {key_from_integer(41), key_from_integer(42), key_from_integer(43)};
    blobs_collection_t collection = database.main();
    failures += expect(collection[keys[0]].assign("a").succeeded());
    failures += expect(collection[keys[1]].erase().succeeded());
    failures += expect(collection[keys[2]].assign("bcd").succeeded());

    // A sentinel right past the offsets catches a read that writes one entry too many
    constexpr ustore_length_t sentinel_k = 0xDEAD;
    std::array<ustore_length_t, count_k + 2> offsets;
    offsets.fill(sentinel_k);
    ustore_octet_t presences = 0;
    ustore_byte_t* values = nullptr;
    ustore_outcome_t outcome {};
    ustore_read_t read {};
    read.database = database;
    read.outcome = &outcome;
    read.count = count_k;
    read.keys = keys.data();
    read.keys_stride = sizeof(ustore_key_t);
    read.presences = &presences;
    read.offsets = offsets.data();
    read.values = &values;
    read.arena = collection.member_arena();
    ustore_read(&read);
    failures += expect_success(outcome);

    failures += expect(presences == 0b101);
    failures += expect_eq(offsets[1] - offsets[0], 1u);
    failures += expect_eq(offsets[2] - offsets[1], 0u);
    failures += expect_eq(offsets[3] - offsets[2], 3u);
    failures += expect_eq(offsets[count_k + 1], sentinel_k);
    if (offsets[3] - offsets[2] == 3)
        failures += expect_eq(std::string_view(reinterpret_cast<char const*>(values) + offsets[2], 3), "bcd");
    return failures;
}

/** A removal takes away the keys it names and leaves the ones a write beside it stored. */
std::size_t database_remove(test_config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;

    std::array<ustore_key_t, 3> const keys {key_from_integer(51), key_from_integer(52), key_from_integer(53)};
    ustore_bytes_cptr_t const value = reinterpret_cast<ustore_bytes_cptr_t>("value");
    ustore_length_t const length = 5;
    ustore_outcome_t outcome {};
    ustore_write_t write {};
    write.database = database;
    write.outcome = &outcome;
    write.count = keys.size();
    write.keys = keys.data();
    write.keys_stride = sizeof(ustore_key_t);
    write.lengths = &length;
    write.values = &value;
    ustore_write(&write);
    failures += expect_success(outcome);

    // The first and the last key go in one batch, which strides over the middle one
    ustore_remove_t remove {};
    remove.database = database;
    remove.outcome = &outcome;
    remove.count = 2;
    remove.keys = keys.data();
    remove.keys_stride = 2 * sizeof(ustore_key_t);
    ustore_remove(&remove);
    failures += expect_success(outcome);

    ustore_octet_t presences = 0;
    ustore_read_t read {};
    read.database = database;
    read.outcome = &outcome;
    read.count = keys.size();
    read.keys = keys.data();
    read.keys_stride = sizeof(ustore_key_t);
    read.presences = &presences;
    ustore_read(&read);
    failures += expect_success(outcome);
    failures += expect(presences == 0b010);
    return failures;
}

/** A rollback withdraws a stage but keeps the writes, so the next commit publishes them. */
case_result_t database_rollback(test_config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_transactions()) return case_result_t::skipped("the engine keeps no transactions", failures);

    ustore_key_t const key = key_from_integer(61);
    transaction_t transaction = *database.transact();
    failures += expect(transaction[key].assign("staged").succeeded());

    ustore_outcome_t outcome {};
    ustore_transaction_stage_t stage {
        .database = database,
        .outcome = &outcome,
        .transaction = transaction,
    };
    ustore_transaction_stage(&stage);
    failures += expect_success(outcome);
    failures += expect(transaction.rollback().succeeded());
    failures += expect(transaction.commit().succeeded());
    blobs_collection_t collection = database.main();
    failures += expect(collection[key].value() == "staged");
    return failures;
}

/** A name resolves at the snapshot it is read at, so a collection created after the snapshot stays missing there. */
case_result_t database_find_at_snapshot(test_config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_snapshots() || !database.supports_collections())
        return case_result_t::skipped("the engine keeps no snapshots of named collections", failures);

    snapshot_t snapshot = *database.snapshot();
    failures += expect(database.create("later").has_value());

    ustore_str_view_t const name = "later";
    ustore_octet_t presence = 0;
    ustore_collection_id_t found = ustore_collection_main_k;
    ustore_outcome_t outcome {};
    ustore_collection_find_t find {};
    find.database = database;
    find.outcome = &outcome;
    find.count = 1;
    find.names = &name;
    find.presences = &presence;
    find.collections = &found;
    find.collections_stride = sizeof(ustore_collection_id_t);
    ustore_collection_find(&find);
    failures += expect_success(outcome);
    failures += expect(presence == 1);

    find.snapshot = snapshot.snapshot();
    ustore_collection_find(&find);
    failures += expect_success(outcome);
    failures += expect(presence == 0);
    return failures;
}

int main(int argc, char** argv) {
    suite_options_t options;
    if (argument_status_t const status = parse_suite_options(argc, argv, options);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = options.totals();

    auto const maybe_directory = test_directory_t::make(options.directory);
    if (!maybe_directory) {
        fmt::println(stderr, "Cannot create a unique directory under {}", options.directory);
        return 1;
    }
    test_config_t const config {options.engine, maybe_directory->path()};

    report(totals, "database.init_refusals", database_init_refusals, config);
    report(totals, "database.validation", database_validation, config);
    report(totals, "database.forged_views", database_forged_views, config);
    report(totals, "database.foreign_handles", database_foreign_handles, config);
    report(totals, "database.executor_refusal", database_executor_refusal, config);
    report(totals, "database.read_offsets", database_read_offsets, config);
    report(totals, "database.remove", database_remove, config);
    report(totals, "database.rollback", database_rollback, config);
    report(totals, "database.find_at_snapshot", database_find_at_snapshot, config);
    return suite_exit_code(totals);
}
