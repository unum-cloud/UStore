/**
 *  @file test/api.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Checks that the C ABI accepts valid argument combinations and rejects malformed ones.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstdio>  // `stderr`

#include <array>       // `std::array`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`

#include <fmt/format.h>

#include <ustore/ustore.hpp>

#include "test.hpp" // `suite_exit_code`, `expect`, `expect_error`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum;

/** Every engine refuses an init without an output, and all but the in-memory one refuse to open without a directory. */
std::size_t database_init_refusals(test_config_t const& config) {
    std::size_t failures = 0;
    ustore_error_t error {};
    open_engine(config.options(), nullptr, &error);
    failures += expect_error(error);

    engine_options_t without_directory = config.options();
    without_directory.directory = nullptr;
    ustore_database_t database = nullptr;
    open_engine(without_directory, &database, &error);
    failures += config.engine() == "ram" ? expect_no_error(error) : expect_error(error);
    ustore_database_free(database);
    return failures;
}

/** Writes, reads, transactions and scans accept valid tasks and reject malformed ones, per engine capability. */
std::size_t database_validation(test_config_t const& config) {
    std::size_t failures = 0;
    database_t database;
    failures += expect(open_database(database, config).succeeded());
    if (failures != 0) return failures;
    blobs_collection_t collection = database.main();
    auto maybe_named_collection = database.find_or_create("col");
    failures += expect(static_cast<bool>(maybe_named_collection) == database.supports_collections());
    auto maybe_transaction = database.transact();
    transaction_t transaction = maybe_transaction.has_value() ? *std::move(maybe_transaction) : transaction_t {};

    constexpr ustore_length_t count = 3;
    std::array<ustore_key_t, count> keys {34, 35, 36};
    std::array<std::uint64_t, count> numbers {34, 35, 36};
    ustore_length_t number_length = sizeof(std::uint64_t);
    std::array<ustore_length_t, count> number_offsets {0, number_length, number_length * 2};
    auto numbers_begin = reinterpret_cast<ustore_bytes_ptr_t>(numbers.data());

    contents_arg_t values {
        .offsets_begin = {number_offsets.data(), sizeof(ustore_length_t)},
        .lengths_begin = {&number_length, 0},
        .contents_begin = {&numbers_begin, 0},
        .count = count,
    };

    using value_extractor_t = contents_arg_extractor<contents_arg_t>;
    auto contents = value_extractor_t {}.contents(values);
    auto offsets = value_extractor_t {}.offsets(values);
    auto lengths = value_extractor_t {}.lengths(values);

    ustore_error_t error {};

#pragma region Writes

    ustore_write_t write_options {
        .db = database,
        .error = &error,
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = collection.member_ptr(),
        .keys = keys.data(),
        .keys_stride = sizeof(ustore_key_t),
        .offsets = offsets.get(),
        .offsets_stride = offsets.stride(),
        .lengths = lengths.get(),
        .lengths_stride = lengths.stride(),
        .values = contents.get(),
        .values_stride = contents.stride(),
    };
    for (ustore_options_t const option : std::array {ustore_options_default_k, ustore_option_write_flush_k}) {
        write_options.options = option;
        ustore_write(&write_options);
        failures += expect_no_error(error);
    }

    if (!database.supports_collections()) {
        std::array<ustore_collection_t, count> collections {1, 2, 3};
        ustore_write_t write_unnamed {
            .db = database,
            .error = &error,
            .arena = collection.member_arena(),
            .tasks_count = count,
            .collections = collections.data(),
            .collections_stride = sizeof(ustore_collection_t),
            .keys = keys.data(),
            .keys_stride = sizeof(ustore_key_t),
            .offsets = offsets.get(),
            .offsets_stride = offsets.stride(),
            .lengths = lengths.get(),
            .lengths_stride = lengths.stride(),
            .values = contents.get(),
            .values_stride = contents.stride(),
        };
        ustore_write(&write_unnamed);
        failures += expect_error(error);

        std::array<ustore_collection_t, count> collections_only_main {0, 0, 0};
        ustore_write_t write_main {
            .db = database,
            .error = &error,
            .arena = collection.member_arena(),
            .tasks_count = count,
            .collections = collections_only_main.data(),
            .collections_stride = sizeof(ustore_collection_t),
            .keys = keys.data(),
            .keys_stride = sizeof(ustore_key_t),
            .offsets = offsets.get(),
            .offsets_stride = offsets.stride(),
            .lengths = lengths.get(),
            .lengths_stride = lengths.stride(),
            .values = contents.get(),
            .values_stride = contents.stride(),
        };
        ustore_write(&write_main);
        failures += expect_no_error(error);
    }

    ustore_write_t write_null_collection {
        .db = database,
        .error = &error,
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = nullptr,
        .keys = keys.data(),
        .keys_stride = sizeof(ustore_key_t),
        .offsets = offsets.get(),
        .offsets_stride = offsets.stride(),
        .lengths = lengths.get(),
        .lengths_stride = lengths.stride(),
        .values = contents.get(),
        .values_stride = contents.stride(),
    };
    ustore_write(&write_null_collection);
    failures += expect_no_error(error);

    if (maybe_named_collection) {
        ustore_write_t write_named {
            .db = database,
            .error = &error,
            .arena = collection.member_arena(),
            .tasks_count = count,
            .collections = maybe_named_collection->member_ptr(),
            .keys = keys.data(),
            .keys_stride = sizeof(ustore_key_t),
            .offsets = offsets.get(),
            .offsets_stride = offsets.stride(),
            .lengths = lengths.get(),
            .lengths_stride = lengths.stride(),
            .values = contents.get(),
            .values_stride = contents.stride(),
        };
        ustore_write(&write_named);
        failures += expect_no_error(error);
    }

    ustore_write_t write {
        .db = database,
        .error = &error,
        .transaction = transaction,
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = collection.member_ptr(),
        .keys = keys.data(),
        .keys_stride = sizeof(ustore_key_t),
        .offsets = offsets.get(),
        .offsets_stride = offsets.stride(),
        .lengths = lengths.get(),
        .lengths_stride = lengths.stride(),
        .values = contents.get(),
        .values_stride = contents.stride(),
    };
    if (database.supports_transactions()) {
        ustore_write(&write);
        failures += expect_no_error(error);

        // A transaction flushes at commit, not at a write
        write.options = ustore_option_write_flush_k;
        ustore_write(&write);
        failures += expect_error(error);
    }

    // No tasks, yet keys are given
    write.transaction = nullptr;
    write.tasks_count = 0;
    write.collections_stride = 0;
    write.options = ustore_options_default_k;
    ustore_write(&write);
    failures += expect_error(error);

    // Tasks without keys
    ustore_write_t write_null_keys {
        .db = database,
        .error = &error,
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = collection.member_ptr(),
        .keys_stride = sizeof(ustore_key_t),
        .offsets = offsets.get(),
        .offsets_stride = offsets.stride(),
        .lengths = lengths.get(),
        .lengths_stride = lengths.stride(),
        .values = contents.get(),
        .values_stride = contents.stride(),
    };
    ustore_write(&write_null_keys);
    failures += expect_error(error);

    ustore_write_t write_wrong_options {
        .db = database,
        .error = &error,
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = collection.member_ptr(),
        .keys = keys.data(),
        .keys_stride = sizeof(ustore_key_t),
        .offsets = offsets.get(),
        .offsets_stride = offsets.stride(),
        .lengths = lengths.get(),
        .lengths_stride = lengths.stride(),
        .values = contents.get(),
        .values_stride = contents.stride(),
    };
    for (ustore_options_t const option : std::array {ustore_option_transaction_dont_watch_k}) {
        write_wrong_options.options = option;
        ustore_write(&write_wrong_options);
        failures += expect_error(error);
    }

#pragma endregion Writes

#pragma region Reads

    ustore_length_t* found_offsets = nullptr;
    ustore_length_t* found_lengths = nullptr;
    ustore_bytes_ptr_t found_values = nullptr;
    ustore_read_t read_without_transaction {
        .db = database,
        .error = &error,
        .arena = collection.member_arena(),
        .tasks_count = count,
        .collections = collection.member_ptr(),
        .keys = keys.data(),
        .keys_stride = sizeof(ustore_key_t),
        .offsets = &found_offsets,
        .lengths = &found_lengths,
        .values = &found_values,
    };
    ustore_read(&read_without_transaction);
    failures += expect_no_error(error);

    ustore_read_t read {
        .db = database,
        .error = &error,
        .transaction = transaction,
        .arena = collection.member_arena(),
        .options = ustore_option_transaction_dont_watch_k,
        .tasks_count = count,
        .collections = collection.member_ptr(),
        .keys = keys.data(),
        .keys_stride = sizeof(ustore_key_t),
        .offsets = &found_offsets,
        .lengths = &found_lengths,
        .values = &found_values,
    };
    if (database.supports_transactions()) {
        ustore_read(&read);
        failures += expect_no_error(error);
    }

    for (ustore_options_t const option :
         std::array {ustore_option_write_flush_k, ustore_option_transaction_dont_watch_k}) {
        read_without_transaction.options = option;
        ustore_read(&read_without_transaction);
        failures += expect_error(error);
    }

#pragma endregion Reads

#pragma region Transactions

    ustore_transaction_t raw_transaction = nullptr;
    ustore_transaction_init_t transaction_init {
        .db = database,
        .error = &error,
        .transaction = &raw_transaction,
    };
    ustore_transaction_init(&transaction_init);
    failures += expect((error.status == ustore_success_k) == database.supports_transactions());
    error = {};

    transaction_init.transaction = nullptr;
    ustore_transaction_init(&transaction_init);
    failures += expect_error(error);

    transaction_init.transaction = &raw_transaction;
    for (ustore_options_t const option :
         std::array {ustore_option_write_flush_k, ustore_option_dont_discard_memory_k}) {
        transaction_init.options = option;
        ustore_transaction_init(&transaction_init);
        failures += expect_error(error);
    }

    ustore_transaction_commit_t transaction_commit {
        .db = database,
        .error = &error,
        .transaction = transaction,
    };
    for (ustore_options_t const option : std::array {ustore_option_dont_discard_memory_k}) {
        transaction_commit.options = option;
        ustore_transaction_commit(&transaction_commit);
        failures += expect_error(error);
    }
    ustore_transaction_free(raw_transaction);

#pragma endregion Transactions

#pragma region Scans

    ustore_key_t* found_keys = nullptr;
    ustore_length_t* found_counts = nullptr;
    ustore_scan_t scan {
        .db = database,
        .error = &error,
        .transaction = transaction,
        .arena = collection.member_arena(),
        .collections = collection.member_ptr(),
        .start_keys = keys.data(),
        .count_limits = &count,
        .offsets = &found_offsets,
        .counts = &found_counts,
        .keys = &found_keys,
    };
    ustore_scan(&scan);
    failures += expect_no_error(error);

    ustore_scan_t scan_without_start_keys {
        .db = database,
        .error = &error,
        .transaction = transaction,
        .arena = collection.member_arena(),
        .collections = collection.member_ptr(),
        .count_limits = &count,
        .offsets = &found_offsets,
        .counts = &found_counts,
        .keys = &found_keys,
    };
    ustore_scan(&scan_without_start_keys);
    failures += expect_error(error);

    ustore_scan_t scan_without_limits {
        .db = database,
        .error = &error,
        .transaction = transaction,
        .arena = collection.member_arena(),
        .collections = collection.member_ptr(),
        .start_keys = keys.data(),
        .offsets = &found_offsets,
        .counts = &found_counts,
        .keys = &found_keys,
    };
    ustore_scan(&scan_without_limits);
    failures += expect_error(error);

#pragma endregion Scans

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
    return suite_exit_code(totals);
}
