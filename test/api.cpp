/**
 *  @file test/api.cpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief Checks that the C ABI accepts valid argument combinations and rejects malformed ones, and keeps its arena,
 *      outcome and executor contracts.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`, `std::uintptr_t`
#include <cstdio>  // `stderr`
#include <cstring> // `std::memcpy`, `std::memset`, `std::memcmp`

#include <algorithm>   // `std::all_of`, `std::sort`
#include <array>       // `std::array`
#include <atomic>      // `std::atomic_ref`
#include <chrono>      // `std::chrono::milliseconds`
#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`
#include <string>      // `std::string`
#include <string_view> // `std::string_view`
#include <thread>      // `std::this_thread::sleep_for`
#include <type_traits> // `std::underlying_type_t`
#include <utility>     // `std::as_const`

#include <fmt/format.h>

#include "ustore/ustore.hpp"

#include "ustore/utilities/ram_buffers.hpp" // `arena_block_t`, `arena_alignment_k`
#include "ustore/utilities/ram_device.hpp"  // `caller_exclusivity_t`, `executor_t`

#include "engine.hpp"  // `engine_t`, `handle_t`, `c_handle`
#include "harness.hpp" // `suite_exit_code`, `expect`, `expect_failure`, `expect_status`, `spin_until`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::utilities;
using namespace unum;

#pragma region Helpers

/** The value every deferred write stores under each of its keys, in static storage, since a deferred call borrows what
 *  its struct points to until final. */
constexpr std::string_view stored_value_k = "value";
strided_strings<std::string_view> const stored_values_k {{&stored_value_k, 0}};

/** Writes @p text under every key of @p keys in the main collection, joining @p transaction when one is given. */
std::size_t write_text(ustore_database_t database, std::span<ustore_key_t const> keys, std::string_view text,
                       ustore_transaction_t transaction = nullptr) {
    strided_strings<std::string_view> const repeated {{&text, 0}};
    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {};
    write.database = database;
    write.outcome = &outcome;
    write.transaction = transaction;
    write.count = keys.size();
    write.keys = {keys.data(), sizeof(ustore_key_t)};
    write.values = repeated.view();
    ustore_blobs_write(&write);
    return expect_success(outcome);
}

/** Submits one deferred write of @c stored_value_k per key of @p keys to @p executor, each into its own outcome. */
void submit_writes(ustore_database_t database, ustore_executor_t executor, std::span<ustore_key_t const> keys,
                   std::span<ustore_outcome_t> outcomes) noexcept {
    for (std::size_t index = 0; index != keys.size(); ++index) {
        ustore_blobs_write_t write {};
        write.database = database;
        write.executor = executor;
        write.outcome = &outcomes[index];
        write.completion = ustore_completion_deferred_k;
        write.count = 1;
        write.keys = {&keys[index], 0};
        write.values = stored_values_k.view();
        ustore_blobs_write(&write);
    }
}

/** An executor of two threads whose deferred table holds @p deferred_limit calls. */
expected<ustore_executor_t> spawn_executor(ustore_size_t deferred_limit) noexcept {
    ustore_executor_t executor = nullptr;
    ustore_outcome_t outcome {};
    ustore_executor_for_ram_t spawn {
        .executor = &executor,
        .outcome = &outcome,
        .threads = 2,
        .deferred_limit = deferred_limit,
    };
    ustore_executor_for_ram(&spawn);
    if (outcome.status != ustore_status_success_k) return status_t {outcome};
    return executor;
}

/** Creates a collection named @p name, which holds @p modality. */
expected<ustore_collection_id_t> create_collection(ustore_database_t database, std::string_view name,
                                                   ustore_modality_t modality = ustore_modality_any_k) noexcept {
    ustore_collection_id_t created {};
    ustore_outcome_t outcome {};
    ustore_collection_create_t create {.database = database,
                                       .outcome = &outcome,
                                       .name = bytes_of(name),
                                       .modality = modality,
                                       .collection = &created};
    ustore_collection_create(&create);
    if (outcome.status != ustore_status_success_k) return status_t {outcome};
    return created;
}

/** Whether a call wrote the output @p tape, whose offsets stay null until it does. */
bool tape_written(ustore_strings_view_t const& tape) noexcept {
    return tape.layout == ustore_strings_u64_tape_k ? tape.data.u64_tape.offsets != nullptr
                                                    : tape.data.u32_tape.offsets != nullptr;
}

/** The columns one read writes into its arena, each null until the read reports it. */
struct read_columns_t {
    ustore_octet_t const* presences = nullptr;
    ustore_strings_view_t values {};

    /** The value of task @p index, empty for a missing key. */
    std::string_view value(std::size_t index) const noexcept { return string_of(tape_string(values, index)); }
};

/** A blocking read of @p keys from the main collection into @p columns inside @p arena, finishing into @p outcome. */
ustore_blobs_read_t read_of(ustore_database_t database, std::span<ustore_key_t const> keys, read_columns_t& columns,
                            ustore_arena_t arena, ustore_outcome_t& outcome) noexcept {
    ustore_blobs_read_t read {};
    read.database = database;
    read.outcome = &outcome;
    read.count = keys.size();
    read.keys = {keys.data(), sizeof(ustore_key_t)};
    read.presences = &columns.presences;
    read.values = &columns.values;
    read.arena = arena;
    return read;
}

#pragma endregion Helpers

/** Every engine refuses an init without an output, and all but the in-memory one refuse to open without a directory. */
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

/** Writes, reads, transactions and scans accept valid tasks and reject malformed ones, per engine capability. */
std::size_t database_validation(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    expected<ustore_collection_id_t> const named = create_collection(database, "col");
    failures += expect(named.has_value() == database.supports_collections());
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
        .collections = {&ustore_collection_main_k, 0},
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

    if (!database.supports_collections()) {
        std::array<ustore_collection_id_t, count> collections {
            {{uuid_from_integer(1)}, {uuid_from_integer(2)}, {uuid_from_integer(3)}}};
        ustore_blobs_write_t write_unnamed = write_head;
        write_unnamed.collections = {collections.data(), sizeof(ustore_collection_id_t)};
        ustore_blobs_write(&write_unnamed);
        failures += expect_failure(outcome);

        std::array<ustore_collection_id_t, count> collections_only_main {
            ustore_collection_main_k, ustore_collection_main_k, ustore_collection_main_k};
        ustore_blobs_write_t write_main = write_head;
        write_main.collections = {collections_only_main.data(), sizeof(ustore_collection_id_t)};
        ustore_blobs_write(&write_main);
        failures += expect_success(outcome);
    }

    ustore_blobs_write_t write_null_collection = write_head;
    write_null_collection.collections = {};
    ustore_blobs_write(&write_null_collection);
    failures += expect_success(outcome);

    if (named) {
        ustore_collection_id_t const named_collection = *named;
        ustore_blobs_write_t write_named = write_head;
        write_named.collections = {&named_collection, 0};
        ustore_blobs_write(&write_named);
        failures += expect_success(outcome);
    }

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

    // Only a transaction records the keys it writes
    ustore_blobs_write_t write_blind_head = write_head;
    write_blind_head.watch = ustore_watch_none_k;
    ustore_blobs_write(&write_blind_head);
    failures += expect_failure(outcome);

#pragma endregion Writes

#pragma region Reads

    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, keys, columns, arena, outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    if (tape_written(columns.values)) failures += expect_eq(columns.value(2).size(), sizeof(std::uint64_t));

    if (transaction) {
        read.view = ustore_transaction_view(transaction, ustore_watch_none_k);
        ustore_blobs_read(&read);
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

    ustore_length_t const* scan_offsets = nullptr;
    ustore_key_t const* found_keys = nullptr;
    ustore_blobs_scan_t scan {
        .database = database,
        .outcome = &outcome,
        .view = ustore_transaction_view(transaction, ustore_watch_keys_k),
        .count = 1,
        .collections = {&ustore_collection_main_k, 0},
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

/** A pointer neither view constructor made, a raw handle with a stray bit above the tag or a bare transaction, is
 *  refused before any engine reads through it. */
std::size_t database_forged_views(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());

    // The read names an arena, so the view, not a missing arena, is what the router refuses
    ustore_key_t const key = key_from_integer(1);
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read {};
    read.database = database;
    read.outcome = &outcome;
    read.count = 1;
    read.keys = {&key, 0};
    read.arena = arena;

    // Handles align past the two tag bits, so the next bit up leaves the tag empty while the pointer stays set
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
    // A transaction is tried first, since it is refused both as a view and as the transaction of a write
    ustore_view_t foreign = nullptr;
    if (maybe_transaction) foreign = ustore_transaction_view(maybe_transaction->transaction(), ustore_watch_keys_k);
    else if (maybe_snapshot) foreign = ustore_snapshot_view(maybe_snapshot->snapshot());
    else return case_result_t::skipped("the other engine hands out no views", failures);

    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    ustore_key_t const key = key_from_integer(1);
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read {};
    read.database = database;
    read.outcome = &outcome;
    read.view = foreign;
    read.count = 1;
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
    write.keys = {&key, 0};
    write.values = stored_values_k.view();
    ustore_blobs_write(&write);
    failures += expect_status(outcome, status_code_t::invalid_argument_k);
    return failures;
}

/** A call that fails zeroes the scalars it reports and leaves the handles it makes @c NULL, whatever they held: a
 *  description, a snapshot and its stamp, a listing's count, a collection, the stamps of a stage and of a commit, a
 *  gist's count, a transaction and an executor. */
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

    // A gist fails on a document that does not parse
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    ustore_key_t const key = key_from_integer(450);
    failures += write_text(database, {&key, 1}, "{not json");
    ustore_size_t paths = 7;
    ustore_strings_view_t fields {};
    ustore_docs_gist_t gist {.database = database,
                             .outcome = &outcome,
                             .count = 1,
                             .keys = {&key, 0},
                             .fields = &fields,
                             .written = &paths,
                             .arena = arena};
    ustore_docs_gist(&gist);
    failures += expect_status(outcome, status_code_t::malformed_k);
    failures += expect_eq(paths, 0u);

    if (database.supports_collections()) {
        failures += expect(create_collection(database, "failed.reports").has_value());
        ustore_collection_id_t collection {uuid_from_integer(0x4242)};
        ustore_collection_create_t create {.database = database,
                                           .outcome = &outcome,
                                           .name = bytes_of("failed.reports"),
                                           .collection = &collection};
        ustore_collection_create(&create);
        failures += expect_failure(outcome);
        failures += expect(collection == ustore_collection_main_k);
    }

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

/** A transaction reset names the database the transaction belongs to, so a reset through another database is refused
 *  before any engine runs, and the transaction keeps serving its own. */
case_result_t database_transaction_reset_elsewhere(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_transactions()) return case_result_t::skipped("the engine keeps no transactions", failures);

    // The in-memory engine opens a second database without a directory, and a disk engine in a directory of its own
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

    ustore_key_t const key = key_from_integer(460);
    failures += write_text(database, {&key, 1}, "value", transaction);
    ustore_transaction_commit_t commit {.database = database, .outcome = &outcome, .transaction = transaction};
    ustore_transaction_commit(&commit);
    failures += expect_success(outcome);
    ustore_transaction_free(transaction);
    return failures;
}

#pragma region Arenas

/** A call needs an arena for its outputs, so a @c NULL one is refused before any engine runs and its outputs are left
 *  @c NULL, and an arena init without an output for the arena is refused too. */
std::size_t arena_null_refused(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;

    ustore_key_t const key = key_from_integer(200);
    failures += write_text(database, {&key, 1}, "value");
    std::uint32_t const sentinel = 0;
    read_columns_t columns {reinterpret_cast<ustore_octet_t const*>(&sentinel), u32_tape(&sentinel, &sentinel)};
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read = read_of(database, {&key, 1}, columns, nullptr, outcome);
    ustore_blobs_read(&read);
    failures += expect_status(outcome, status_code_t::invalid_argument_k);
    failures += expect(!columns.presences && !tape_written(columns.values));

    ustore_arena_init_t init {.outcome = &outcome};
    ustore_arena_init(&init);
    failures += expect_status(outcome, status_code_t::invalid_argument_k);
    return failures;
}

/** One arena serves a read and then a scan, each call rewinding it and writing whole outputs. */
std::size_t arena_reuse(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;

    // Three present keys and a missing one past them, which no other case writes between
    std::array<ustore_key_t, 4> const keys {key_from_integer(300), key_from_integer(301), key_from_integer(302),
                                            key_from_integer(303)};
    failures += write_text(database, std::span {keys}.first(3), "value");
    read_columns_t columns;
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read = read_of(database, keys, columns, arena, outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(columns.presences && tape_written(columns.values));
    if (failures != 0) return failures;
    failures += expect_eq(columns.presences[0], 0b0111);
    failures += expect_eq(columns.value(2), "value");
    failures += expect(columns.value(3).empty());

    ustore_length_t const* offsets = nullptr;
    ustore_key_t const* found = nullptr;
    ustore_blobs_scan_t scan {
        .database = database,
        .outcome = &outcome,
        .count = 1,
        .start_keys = {&keys[0], 0},
        .offsets = &offsets,
        .keys = &found,
        .arena = arena,
        .wanted = 3,
    };
    ustore_blobs_scan(&scan);
    failures += expect_success(outcome);
    failures += expect(offsets && found && offsets[0] == 0 && offsets[1] == 3);
    if (found) failures += expect(std::memcmp(found, keys.data(), 3 * sizeof(ustore_key_t)) == 0);
    return failures;
}

/** A call that fails leaves every output @c NULL, blocking or deferred, whatever the outputs held before. */
std::size_t arena_failed_outputs_null(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [executor, spawn_status] = spawn_executor(0);
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());

    // No collection carries this identifier, so every engine fails the read
    ustore_key_t const key = key_from_integer(500);
    ustore_collection_id_t const missing {uuid_from_integer(0x4242)};
    std::uint32_t const sentinel = 0;
    read_columns_t const stale {reinterpret_cast<ustore_octet_t const*>(&sentinel), u32_tape(&sentinel, &sentinel)};
    for (ustore_completion_t const completion :
         std::array {ustore_completion_blocking_k, ustore_completion_deferred_k}) {
        read_columns_t columns = stale;
        ustore_outcome_t outcome {};
        ustore_blobs_read_t read = read_of(database, {&key, 1}, columns, arena, outcome);
        read.executor = executor;
        read.completion = completion;
        read.collections = {&missing, 0};
        ustore_blobs_read(&read);
        ustore_status_t const status = ustore_outcome_wait(&outcome);
        failures += expect(status != ustore_status_success_k);
        failures += expect(!columns.presences && !tape_written(columns.values));
    }

    // A scan without an output for its keys is refused, and its offsets are left null too
    ustore_length_t const* offsets = reinterpret_cast<ustore_length_t const*>(&sentinel);
    ustore_outcome_t outcome {};
    ustore_blobs_scan_t scan {
        .database = database,
        .outcome = &outcome,
        .count = 1,
        .offsets = &offsets,
        .arena = arena,
        .wanted = 1,
    };
    ustore_blobs_scan(&scan);
    failures += expect_failure(outcome);
    failures += expect(!offsets);
    ustore_executor_free(executor);
    return failures;
}

/** Whether @p output starts at a multiple of 64 bytes, where every output of an arena starts. */
bool aligned(void const* output) noexcept { return reinterpret_cast<std::uintptr_t>(output) % 64 == 0; }

/** Every output starts aligned to 64 bytes, as Apache Arrow buffers do, so typed columns read in place: the columns
 *  of a gather, of a read and the bounds of a measurement. */
std::size_t arena_aligned_outputs(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;

    // Three documents, the last without a height or a name, so every bitmap of a column differs
    std::array<ustore_key_t, 3> const keys {key_from_integer(800), key_from_integer(801), key_from_integer(802)};
    std::array<std::string_view, 3> const documents {R"({"born":1815,"height":1.5,"name":"ada"})",
                                                     R"({"born":1906,"height":1.6,"name":"grace"})",
                                                     R"({"born":1918})"};
    strided_strings<std::string_view> const values {strided(documents).begin()};
    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {};
    write.database = database;
    write.outcome = &outcome;
    write.count = keys.size();
    write.keys = {keys.data(), sizeof(ustore_key_t)};
    write.values = values.view();
    ustore_blobs_write(&write);
    failures += expect_success(outcome);

    std::array<std::string_view, 3> const fields {"born", "height", "name"};
    strided_strings<std::string_view> const fields_strings {strided(fields).begin()};
    std::array<ustore_docs_field_type_t, 3> const types {ustore_docs_field_type_u64_k, ustore_docs_field_type_f64_k,
                                                         ustore_docs_field_type_str_k};
    ustore_docs_column_view_t const* columns = nullptr;
    ustore_docs_gather_t gather {
        .database = database,
        .outcome = &outcome,
        .count = keys.size(),
        .keys = {keys.data(), sizeof(ustore_key_t)},
        .fields_count = fields.size(),
        .fields = fields_strings.view(),
        .columns = &columns,
        .arena = arena,
        .outputs_types = types.data(),
    };
    ustore_docs_gather(&gather);
    failures += expect_success(outcome);
    failures += expect(columns != nullptr);
    if (failures != 0) return failures;
    for (std::size_t field = 0; field != fields.size(); ++field) {
        ustore_docs_column_view_t const& column = columns[field];
        failures += expect(aligned(column.validities) && aligned(column.conversions) && aligned(column.collisions));
        failures += expect(aligned(column.scalars));
    }
    failures += expect(aligned(columns[2].strings.data.u32_tape.offsets) &&
                       aligned(columns[2].strings.data.u32_tape.data));
    auto const* const born = reinterpret_cast<std::uint64_t const*>(columns[0].scalars);
    auto const* const heights = reinterpret_cast<double const*>(columns[1].scalars);
    failures += expect_eq(born[2], 1918u);
    failures += expect_eq(heights[1], 1.6);
    failures += expect_eq(columns[1].validities[0], 0b011);
    failures += expect_eq(tape_string(columns[2].strings, 0).length, 4u);

    read_columns_t read_columns;
    ustore_blobs_read_t read = read_of(database, keys, read_columns, arena, outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(read_columns.presences && tape_written(read_columns.values));
    failures += expect(aligned(read_columns.presences) && aligned(read_columns.values.data.u32_tape.offsets) &&
                       aligned(read_columns.values.data.u32_tape.data));

    ustore_key_t const lowest = key_from_integer(0);
    ustore_key_t const highest = key_from_integer(1000);
    std::array<ustore_size_t const*, 6> bounds {};
    ustore_blobs_measure_t measure {
        .database = database,
        .outcome = &outcome,
        .count = 1,
        .start_keys = {&lowest, 0},
        .end_keys = {&highest, 0},
        .min_cardinalities = &bounds[0],
        .max_cardinalities = &bounds[1],
        .min_value_bytes = &bounds[2],
        .max_value_bytes = &bounds[3],
        .min_space_usages = &bounds[4],
        .max_space_usages = &bounds[5],
        .arena = arena,
    };
    ustore_blobs_measure(&measure);
    failures += expect_success(outcome);
    for (ustore_size_t const* bound : bounds) failures += expect(bound && aligned(bound));
    return failures;
}

/** Where one output of a call starts in its arena and how many bytes it takes. */
struct output_extent_t {
    void const* begin = nullptr;
    std::size_t bytes = 0;
};

/** The offsets and then the bytes of an output @p tape of @p count strings, as two outputs of its call. */
std::array<output_extent_t, 2> tape_extents(ustore_strings_view_t const& tape, std::size_t count) noexcept {
    if (tape.layout == ustore_strings_u64_tape_k)
        return {{{tape.data.u64_tape.offsets, (count + 1) * sizeof(std::uint64_t)},
                 {tape.data.u64_tape.data, tape.data.u64_tape.offsets[count]}}};
    return {{{tape.data.u32_tape.offsets, (count + 1) * sizeof(std::uint32_t)},
             {tape.data.u32_tape.data, tape.data.u32_tape.offsets[count]}}};
}

/** Checks that the non-empty @p outputs tile every block of @p arena from its header to the last byte it handed out,
 *  each where alignment puts the allocation after the one before, so the arena holds nothing else: no growth slack,
 *  and no partial copy of an output that outgrew a block. */
std::size_t expect_only_outputs(ustore_arena_t arena, std::span<output_extent_t> outputs) {
    std::size_t failures = 0;
    auto const address = [](void const* pointer) noexcept { return reinterpret_cast<std::uintptr_t>(pointer); };
    std::sort(outputs.begin(), outputs.end(), [&](output_extent_t const& left, output_extent_t const& right) noexcept {
        return address(left.begin) < address(right.begin);
    });
    std::size_t tiled = 0;
    for (arena_block_t const* block = arena->newest; block; block = block->previous) {
        std::uintptr_t const block_begin = address(block);
        std::uintptr_t end = block_begin + sizeof(arena_block_t);
        for (output_extent_t const& output : outputs) {
            if (output.bytes == 0 || address(output.begin) < block_begin ||
                address(output.begin) >= block_begin + block->capacity)
                continue;
            failures += expect_eq(address(output.begin), round_up_to_multiple<std::uintptr_t>(end, arena_alignment_k));
            end = address(output.begin) + output.bytes;
            ++tiled;
        }
        failures += expect_eq(end, block_begin + block->used);
    }
    failures += expect_eq(tiled, static_cast<std::size_t>(std::count_if(
                                     outputs.begin(), outputs.end(),
                                     [](output_extent_t const& output) noexcept { return output.bytes != 0; })));
    return failures;
}

/** Checks that the @p outputs of a one-off @p arena's first call outgrew its first block, which spans one page, into
 *  a second, and still tile both. */
std::size_t expect_outgrown_outputs(ustore_arena_t arena, std::span<output_extent_t> outputs) {
    std::size_t failures = 0;
    arena_block_t const* const newest = arena->newest;
    failures += expect(newest && newest->previous && !newest->previous->previous);
    if (failures != 0) return failures;
    failures += expect_eq(newest->previous->capacity, arena_page_bytes_k);
    return failures + expect_only_outputs(arena, outputs);
}

/** Every call stages what grows in scratch and copies its outputs in at their final lengths, so the arena it fills
 *  holds exactly its outputs, at the head and through a transaction alike: a docs gather, a graph read, a blob read, a
 *  docs read by field and a gist in an arena kept across them, then a collection list, a key scan, a strings scan, a
 *  gather of long strings and a read of many edges, each in a one-off arena its outputs outgrow. */
std::size_t arena_outputs_only(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    auto [graph, graph_status] = create_collection(database, "outputs.graph", ustore_modality_graph_k);
    failures += expect(graph_status.succeeded());
    if (failures != 0) return failures;

    // Every document carries a long biography no output holds, which any intermediate left in the arena would show
    constexpr std::size_t docs_count = 4;
    std::array<ustore_key_t, docs_count> const keys {key_from_integer(1100), key_from_integer(1101),
                                                     key_from_integer(1102), key_from_integer(1103)};
    std::array<std::string, docs_count> const documents {
        fmt::format(R"({{"born":1815,"name":"ada","bio":"{:x>600}"}})", ""),
        fmt::format(R"({{"born":1906,"name":"grace","bio":"{:x>600}"}})", ""),
        fmt::format(R"({{"born":1918,"bio":"{:x>600}"}})", ""),
        fmt::format(R"({{"name":"edsger","bio":"{:x>600}"}})", ""),
    };
    strided_strings<std::string> const values {strided(documents).begin()};
    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {};
    write.database = database;
    write.outcome = &outcome;
    write.count = docs_count;
    write.keys = {keys.data(), sizeof(ustore_key_t)};
    write.values = values.view();
    ustore_blobs_write(&write);
    failures += expect_success(outcome);

    // Three edges between three vertices, and a fourth vertex no edge reaches, which reads as missing
    std::array<ustore_key_t, 3> const sources {key_from_integer(1300), key_from_integer(1300), key_from_integer(1301)};
    std::array<ustore_key_t, 3> const targets {key_from_integer(1301), key_from_integer(1302), key_from_integer(1302)};
    ustore_graph_write_edges_t write_edges {
        .database = database,
        .outcome = &outcome,
        .count = sources.size(),
        .collections = {&graph, 0},
        .sources = {sources.data(), sizeof(ustore_key_t)},
        .targets = {targets.data(), sizeof(ustore_key_t)},
    };
    ustore_graph_write_edges(&write_edges);
    failures += expect_success(outcome);
    std::array<ustore_key_t, 4> const vertices {key_from_integer(1300), key_from_integer(1301), key_from_integer(1302),
                                                key_from_integer(1399)};

    // A hub of a hundred edges, three hundred keys and strings, and a long name, each outgrowing one page
    constexpr std::size_t fanned_count = 100;
    constexpr std::size_t many_count = 300;
    ustore_key_t const hub = key_from_integer(1400);
    std::array<ustore_key_t, fanned_count> fanned {};
    for (std::size_t index = 0; index != fanned_count; ++index) fanned[index] = key_from_integer(1401 + index);
    ustore_graph_write_edges_t write_fan {
        .database = database,
        .outcome = &outcome,
        .count = fanned_count,
        .collections = {&graph, 0},
        .sources = {&hub, 0},
        .targets = {fanned.data(), sizeof(ustore_key_t)},
    };
    ustore_graph_write_edges(&write_fan);
    failures += expect_success(outcome);
    std::array<ustore_key_t, many_count> scanned {};
    for (std::size_t index = 0; index != many_count; ++index) scanned[index] = key_from_integer(5000 + index);
    failures += write_text(database, scanned, "value");
    auto [strings, strings_status] = create_collection(database, "outputs.strings", ustore_modality_strings_k);
    failures += expect(strings_status.succeeded());
    std::array<std::string, many_count> texts {};
    for (std::size_t index = 0; index != many_count; ++index) texts[index] = fmt::format("outputs.string.{:03}", index);
    strided_strings<std::string> const text_strings {strided(std::as_const(texts)).begin()};
    ustore_strings_write_t write_strings {
        .database = database,
        .outcome = &outcome,
        .count = many_count,
        .collections = {&strings, 0},
        .strings = text_strings.view(),
        .values = stored_values_k.view(),
    };
    ustore_strings_write(&write_strings);
    failures += expect_success(outcome);
    std::string const long_name(5000, 'n');
    failures += expect(create_collection(database, long_name).has_value());
    if (failures != 0) return failures;

    // The listing exports every collection, earlier cases' included, the long name outgrowing the first page
    {
        auto [one_off, one_off_status] = arena_t::make();
        failures += expect(one_off_status.succeeded());
        ustore_size_t written = 0;
        ustore_collection_id_t const* ids = nullptr;
        ustore_modality_t const* modalities = nullptr;
        ustore_strings_view_t names {};
        ustore_collection_list_t list {
            .database = database,
            .outcome = &outcome,
            .collections = &ids,
            .modalities = &modalities,
            .names = &names,
            .written = &written,
            .arena = one_off,
        };
        ustore_collection_list(&list);
        failures += expect_success(outcome);
        failures += expect(written >= 3 && ids && modalities && tape_written(names));
        if (failures != 0) return failures;
        std::array<output_extent_t, 2> const names_tape = tape_extents(names, written);
        std::array<output_extent_t, 4> listed {{
            {ids, written * sizeof(ustore_collection_id_t)},
            {modalities, written * sizeof(ustore_modality_t)},
            names_tape[0],
            names_tape[1],
        }};
        failures += expect_outgrown_outputs(one_off, listed);
    }

    // An engine without transactions reads at the head alone
    ustore_transaction_t transaction = nullptr;
    ustore_transaction_init_t begin {.transaction = &transaction, .database = database, .outcome = &outcome};
    ustore_transaction_init(&begin);
    outcome = {};
    std::array<ustore_view_t, 2> const views {nullptr, ustore_transaction_view(transaction, ustore_watch_keys_k)};
    for (ustore_view_t const view : std::span {views}.first(transaction ? 2 : 1)) {
        std::array<std::string_view, 2> const fields {"born", "name"};
        strided_strings<std::string_view> const fields_strings {strided(fields).begin()};
        std::array<ustore_docs_field_type_t, 2> const types {ustore_docs_field_type_u64_k,
                                                             ustore_docs_field_type_str_k};
        ustore_docs_column_view_t const* columns = nullptr;
        ustore_docs_gather_t gather {
            .database = database,
            .outcome = &outcome,
            .view = view,
            .count = docs_count,
            .keys = {keys.data(), sizeof(ustore_key_t)},
            .fields_count = fields.size(),
            .fields = fields_strings.view(),
            .columns = &columns,
            .arena = arena,
            .outputs_types = types.data(),
        };
        ustore_docs_gather(&gather);
        failures += expect_success(outcome);
        failures += expect(columns != nullptr);
        if (!columns) continue;
        ustore_docs_column_view_t const& born = columns[0];
        ustore_docs_column_view_t const& name = columns[1];
        auto const* const years = reinterpret_cast<std::uint64_t const*>(born.scalars);
        failures += expect_eq(years[2], 1918u);
        failures += expect_eq(std::string_view {reinterpret_cast<char const*>(tape_string(name.strings, 3).data)},
                              "edsger");
        std::size_t const bitmap_bytes = divide_round_up(docs_count, bits_in_byte_k);
        std::array<output_extent_t, 2> const name_tape = tape_extents(name.strings, docs_count);
        std::array<output_extent_t, 10> gathered {{
            {columns, fields.size() * sizeof(ustore_docs_column_view_t)},
            {born.validities, bitmap_bytes},
            {born.conversions, bitmap_bytes},
            {born.collisions, bitmap_bytes},
            {born.scalars, docs_count * sizeof(std::uint64_t)},
            {name.validities, bitmap_bytes},
            {name.conversions, bitmap_bytes},
            {name.collisions, bitmap_bytes},
            name_tape[0],
            name_tape[1],
        }};
        failures += expect_only_outputs(arena, gathered);

        // The missing vertex spans no edges, so the offsets close at six
        ustore_vertex_degree_t const* degrees = nullptr;
        ustore_length_t const* edges_offsets = nullptr;
        ustore_key_t const* edges = nullptr;
        ustore_graph_read_edges_t read_edges {
            .database = database,
            .outcome = &outcome,
            .view = view,
            .count = vertices.size(),
            .collections = {&graph, 0},
            .vertices = {vertices.data(), sizeof(ustore_key_t)},
            .degrees_per_vertex = &degrees,
            .edges_offsets = &edges_offsets,
            .edges_per_vertex = &edges,
            .arena = arena,
        };
        ustore_graph_read_edges(&read_edges);
        failures += expect_success(outcome);
        failures += expect(degrees && edges_offsets && edges);
        if (!degrees || !edges_offsets || !edges) continue;
        failures += expect(degrees[0] == 2 && degrees[1] == 2 && degrees[2] == 2);
        failures += expect_eq(degrees[3], ustore_vertex_degree_missing_k);
        failures += expect(edges_offsets[0] == 0 && edges_offsets[1] == 2 && edges_offsets[2] == 4);
        failures += expect(edges_offsets[3] == 6 && edges_offsets[4] == 6);
        std::array<output_extent_t, 3> read {{
            {degrees, vertices.size() * sizeof(ustore_vertex_degree_t)},
            {edges_offsets, (vertices.size() + 1) * sizeof(ustore_length_t)},
            {edges, 6 * 3 * sizeof(ustore_key_t)},
        }};
        failures += expect_only_outputs(arena, read);

        // A blob read copies every document once, however long, and leaves no room for more behind them
        read_columns_t blobs;
        ustore_blobs_read_t read_blobs = read_of(database, keys, blobs, arena, outcome);
        read_blobs.view = view;
        ustore_blobs_read(&read_blobs);
        failures += expect_success(outcome);
        failures += expect(blobs.presences && tape_written(blobs.values));
        if (!blobs.presences || !tape_written(blobs.values)) continue;
        failures += expect_eq(blobs.value(3), documents[3]);
        std::array<output_extent_t, 2> const blobs_tape = tape_extents(blobs.values, docs_count);
        std::array<output_extent_t, 3> blob_columns {{
            {blobs.presences, bitmap_bytes},
            blobs_tape[0],
            blobs_tape[1],
        }};
        failures += expect_only_outputs(arena, blob_columns);

        // A read by field exports the names alone, the third document missing one
        read_columns_t names;
        std::string_view const name_field = "name";
        strided_strings<std::string_view> const name_fields {{&name_field, 0}};
        ustore_docs_read_t read_names {
            .database = database,
            .outcome = &outcome,
            .view = view,
            .count = docs_count,
            .keys = {keys.data(), sizeof(ustore_key_t)},
            .fields = name_fields.view(),
            .presences = &names.presences,
            .values = &names.values,
            .arena = arena,
            .outputs_type = ustore_docs_field_type_str_k,
        };
        ustore_docs_read(&read_names);
        failures += expect_success(outcome);
        failures += expect(names.presences && tape_written(names.values));
        if (!names.presences || !tape_written(names.values)) continue;
        failures += expect_eq(names.presences[0], 0b1011);
        failures += expect_eq(names.value(1), "grace");
        failures += expect_eq(names.value(3), "edsger");
        std::array<output_extent_t, 2> const names_tape = tape_extents(names.values, docs_count);
        failures += expect_eq(names_tape[1].bytes, std::string_view {"adagraceedsger"}.size());
        std::array<output_extent_t, 3> name_columns {{
            {names.presences, bitmap_bytes},
            names_tape[0],
            names_tape[1],
        }};
        failures += expect_only_outputs(arena, name_columns);

        // A gist finds three paths, which leave no partial copies of their offsets or of themselves behind
        ustore_size_t written = 0;
        ustore_strings_view_t paths {};
        ustore_docs_gist_t gist {
            .database = database,
            .outcome = &outcome,
            .view = view,
            .count = docs_count,
            .keys = {keys.data(), sizeof(ustore_key_t)},
            .fields = &paths,
            .written = &written,
            .arena = arena,
        };
        ustore_docs_gist(&gist);
        failures += expect_success(outcome);
        failures += expect_eq(written, 3u);
        failures += expect(tape_written(paths));
        if (!tape_written(paths)) continue;
        failures += expect_eq(string_of(tape_string(paths, 1)), "/name");
        std::array<output_extent_t, 2> gist_columns = tape_extents(paths, written);
        failures += expect_only_outputs(arena, gist_columns);

        // Three hundred keys outgrow the first page of a one-off arena, which only the offsets fit
        {
            auto [one_off, one_off_status] = arena_t::make();
            failures += expect(one_off_status.succeeded());
            ustore_length_t const* offsets = nullptr;
            ustore_key_t const* found = nullptr;
            ustore_blobs_scan_t scan {
                .database = database,
                .outcome = &outcome,
                .view = view,
                .count = 1,
                .start_keys = {&scanned[0], 0},
                .offsets = &offsets,
                .keys = &found,
                .arena = one_off,
                .wanted = many_count,
            };
            ustore_blobs_scan(&scan);
            failures += expect_success(outcome);
            failures += expect(offsets && found);
            if (!offsets || !found) continue;
            failures += expect_eq(offsets[1], many_count);
            failures += expect(std::memcmp(found, scanned.data(), sizeof(scanned)) == 0);
            std::array<output_extent_t, 2> scan_columns {{
                {offsets, 2 * sizeof(ustore_length_t)},
                {found, many_count * sizeof(ustore_key_t)},
            }};
            failures += expect_outgrown_outputs(one_off, scan_columns);
        }

        // Three hundred strings outgrow it too, after their offsets
        {
            auto [one_off, one_off_status] = arena_t::make();
            failures += expect(one_off_status.succeeded());
            ustore_length_t const* offsets = nullptr;
            ustore_strings_view_t found {};
            ustore_strings_scan_t scan {
                .database = database,
                .outcome = &outcome,
                .view = view,
                .count = 1,
                .collections = {&strings, 0},
                .offsets = &offsets,
                .strings = &found,
                .arena = one_off,
                .wanted = many_count,
            };
            ustore_strings_scan(&scan);
            failures += expect_success(outcome);
            failures += expect(offsets && tape_written(found));
            if (!offsets || !tape_written(found)) continue;
            failures += expect_eq(offsets[1], many_count);
            failures += expect_eq(string_of(tape_string(found, many_count - 1)), texts[many_count - 1]);
            std::array<output_extent_t, 2> const found_tape = tape_extents(found, many_count);
            std::array<output_extent_t, 3> scan_columns {{
                {offsets, 2 * sizeof(ustore_length_t)},
                found_tape[0],
                found_tape[1],
            }};
            failures += expect_outgrown_outputs(one_off, scan_columns);
        }

        // Gathering the biographies twice, the second string column outgrows it
        {
            auto [one_off, one_off_status] = arena_t::make();
            failures += expect(one_off_status.succeeded());
            std::string_view const biography = "bio";
            strided_strings<std::string_view> const biographies {{&biography, 0}};
            std::array<ustore_docs_field_type_t, 2> const text_types {ustore_docs_field_type_str_k,
                                                                      ustore_docs_field_type_str_k};
            ustore_docs_column_view_t const* columns = nullptr;
            ustore_docs_gather_t gather {
                .database = database,
                .outcome = &outcome,
                .view = view,
                .count = docs_count,
                .keys = {keys.data(), sizeof(ustore_key_t)},
                .fields_count = text_types.size(),
                .fields = biographies.view(),
                .columns = &columns,
                .arena = one_off,
                .outputs_types = text_types.data(),
            };
            ustore_docs_gather(&gather);
            failures += expect_success(outcome);
            failures += expect(columns != nullptr);
            if (!columns) continue;
            std::size_t const bitmap_bytes = divide_round_up(docs_count, bits_in_byte_k);
            std::array<output_extent_t, 11> gathered {};
            gathered[0] = {columns, text_types.size() * sizeof(ustore_docs_column_view_t)};
            for (std::size_t field = 0; field != text_types.size(); ++field) {
                ustore_docs_column_view_t const& column = columns[field];
                std::array<output_extent_t, 2> const column_tape = tape_extents(column.strings, docs_count);
                failures += expect_eq(column_tape[1].bytes, docs_count * 601);
                gathered[1 + 5 * field] = {column.validities, bitmap_bytes};
                gathered[2 + 5 * field] = {column.conversions, bitmap_bytes};
                gathered[3 + 5 * field] = {column.collisions, bitmap_bytes};
                gathered[4 + 5 * field] = column_tape[0];
                gathered[5 + 5 * field] = column_tape[1];
            }
            failures += expect_outgrown_outputs(one_off, gathered);
        }

        // A hundred edges of the hub outgrow it after the degree and the offsets
        {
            auto [one_off, one_off_status] = arena_t::make();
            failures += expect(one_off_status.succeeded());
            ustore_vertex_degree_t const* degrees = nullptr;
            ustore_length_t const* edges_offsets = nullptr;
            ustore_key_t const* edges = nullptr;
            ustore_graph_read_edges_t read_fan {
                .database = database,
                .outcome = &outcome,
                .view = view,
                .count = 1,
                .collections = {&graph, 0},
                .vertices = {&hub, 0},
                .degrees_per_vertex = &degrees,
                .edges_offsets = &edges_offsets,
                .edges_per_vertex = &edges,
                .arena = one_off,
            };
            ustore_graph_read_edges(&read_fan);
            failures += expect_success(outcome);
            failures += expect(degrees && edges_offsets && edges);
            if (!degrees || !edges_offsets || !edges) continue;
            failures += expect_eq(degrees[0], fanned_count);
            std::array<output_extent_t, 3> fan_columns {{
                {degrees, sizeof(ustore_vertex_degree_t)},
                {edges_offsets, 2 * sizeof(ustore_length_t)},
                {edges, fanned_count * 3 * sizeof(ustore_key_t)},
            }};
            failures += expect_outgrown_outputs(one_off, fan_columns);
        }
    }
    ustore_transaction_free(transaction);
    return failures;
}

#pragma endregion Arenas

#pragma region Executors

/** Reports progress once and answers, standing in for a long scan whose progress callback a case holds open. */
status_t scan_reporting_progress(ustore_blobs_scan_t const& arguments, view_t const&) noexcept {
    if (arguments.progress && arguments.progress(0, 1, arguments.progress_state) == ustore_cancel_k)
        return {status_code_t::cancelled_k, "The progress callback stopped the scan"};
    return {};
}

/** Leaves the stand-in database to the case holding it. */
void leave_database(ustore_database_t) noexcept {}

/** An engine serving only scans, each reporting its progress once, so a call stays unfinished while a case needs. */
engine_t const progress_engine_k {.blobs_scan = &scan_reporting_progress, .database_free = &leave_database};

/** How far a scan held inside its progress callback got. */
enum class hold_stage_t : std::uint32_t {
    submitted_k,
    entered_k,
    released_k,
    returned_k,
};

/** A deferred scan on the stand-in engine, its callback holding it until the case releases it, with what a drain of
 *  its own executor answered from inside that callback. */
struct held_scan_t {
    handle_t database {&progress_engine_k};
    ustore_executor_t executor = nullptr;
    hold_stage_t stage = hold_stage_t::submitted_k;
    ustore_status_t drained = ustore_status_success_k;
    ustore_outcome_t outcome {};
    ustore_key_t start_key {};
    arena_t arena;
    ustore_blobs_scan_t scan {};
};

/** Drains its own executor, then holds the scan until released, lingering so a waiter is waiting when it finishes. */
ustore_continuation_t hold_until_released(ustore_size_t, ustore_size_t, void* state) noexcept {
    held_scan_t& held = *static_cast<held_scan_t*>(state);
    ustore_outcome_t drained {};
    ustore_executor_drain_t drain {.executor = held.executor, .outcome = &drained};
    ustore_executor_drain(&drain);
    held.drained = drained.status;

    std::atomic_ref<hold_stage_t> const stage {held.stage};
    stage.store(hold_stage_t::entered_k, std::memory_order_release);
    stage.wait(hold_stage_t::entered_k, std::memory_order_acquire);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stage.store(hold_stage_t::returned_k, std::memory_order_release);
    return ustore_continue_k;
}

/** Submits @p held to @p executor and waits until its callback holds it, so later deferred calls wait behind it. */
std::size_t submit_held(held_scan_t& held, ustore_executor_t executor) noexcept {
    auto [arena, arena_status] = arena_t::make(executor);
    if (arena_status.failed()) return 1;
    held.arena = std::move(arena);
    held.executor = executor;
    held.scan = {
        .database = c_handle<ustore_database_t>(&held.database),
        .executor = executor,
        .outcome = &held.outcome,
        .completion = ustore_completion_deferred_k,
        .count = 1,
        .start_keys = {&held.start_key, 0},
        .arena = held.arena,
        .progress = &hold_until_released,
        .progress_state = &held,
    };
    ustore_blobs_scan(&held.scan);
    std::atomic_ref<hold_stage_t> const stage {held.stage};
    return expect(
        spin_until([&stage]() noexcept { return stage.load(std::memory_order_acquire) == hold_stage_t::entered_k; }));
}

/** Lets the callback of @p held return, and waits until the scan finishes. */
std::size_t release_held(held_scan_t& held) noexcept {
    std::atomic_ref<hold_stage_t> const stage {held.stage};
    stage.store(hold_stage_t::released_k, std::memory_order_release);
    stage.notify_all();
    ustore_outcome_wait(&held.outcome);
    return expect_success(held.outcome);
}

/** A deferred write and then a deferred read into an arena the executor made round-trip the values, and a blocking
 *  read into an arena made for the calling thread reads them back as well. */
std::size_t executor_deferred_round_trip(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [executor, spawn_status] = spawn_executor(0);
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;
    auto [executor_arena, executor_arena_status] = arena_t::make(executor);
    auto [calling_arena, calling_arena_status] = arena_t::make();
    failures += expect(executor_arena_status.succeeded() && calling_arena_status.succeeded());

    std::array<ustore_key_t, 3> const keys {key_from_integer(71), key_from_integer(72), key_from_integer(73)};
    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {};
    write.database = database;
    write.executor = executor;
    write.outcome = &outcome;
    write.completion = ustore_completion_deferred_k;
    write.count = keys.size();
    write.keys = {keys.data(), sizeof(ustore_key_t)};
    write.values = stored_values_k.view();
    ustore_blobs_write(&write);
    failures += expect(ustore_outcome_wait(&outcome) == ustore_status_success_k);

    read_columns_t deferred_columns;
    ustore_blobs_read_t deferred_read = read_of(database, keys, deferred_columns, executor_arena, outcome);
    deferred_read.executor = executor;
    deferred_read.completion = ustore_completion_deferred_k;
    ustore_blobs_read(&deferred_read);
    failures += expect(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    read_columns_t blocking_columns;
    ustore_blobs_read_t blocking_read = read_of(database, keys, blocking_columns, calling_arena, outcome);
    ustore_blobs_read(&blocking_read);
    failures += expect_success(outcome);

    for (read_columns_t const& columns : {deferred_columns, blocking_columns}) {
        failures += expect(columns.presences && tape_written(columns.values));
        if (!columns.presences || !tape_written(columns.values)) continue;
        failures += expect_eq(columns.presences[0], 0b111);
        for (std::size_t index = 0; index != keys.size(); ++index) failures += expect_eq(columns.value(index), "value");
    }
    ustore_executor_free(executor);
    return failures;
}

/** A deferred call runs from the copy its submission took, so the caller may scribble over its argument struct at
 *  once and the library never writes it back, while polling through @c ustore_outcome_status reads the call deferred
 *  until it is final. */
std::size_t executor_copied_submission(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [executor, spawn_status] = spawn_executor(0);
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());

    std::array<ustore_key_t, 3> const keys {key_from_integer(400), key_from_integer(401), key_from_integer(402)};
    failures += write_text(database, keys, "value");

    // The read waits behind the held scan, so it cannot run before its struct is scribbled over
    held_scan_t held;
    failures += submit_held(held, executor);
    read_columns_t columns;
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read = read_of(database, keys, columns, arena, outcome);
    read.executor = executor;
    read.completion = ustore_completion_deferred_k;
    ustore_blobs_read(&read);
    std::memset(&read, 0xA5, sizeof(read));
    failures += expect(ustore_outcome_status(&outcome) == ustore_status_deferred_k);

    failures += release_held(held);
    failures += expect(
        spin_until([&outcome]() noexcept { return ustore_outcome_status(&outcome) != ustore_status_deferred_k; }));
    failures += expect(ustore_outcome_status(&outcome) == ustore_status_success_k);
    failures += expect(columns.presences && tape_written(columns.values));
    if (columns.presences && tape_written(columns.values)) {
        failures += expect_eq(columns.presences[0], 0b111);
        for (std::size_t index = 0; index != keys.size(); ++index) failures += expect_eq(columns.value(index), "value");
    }
    auto const* const scribbled = reinterpret_cast<unsigned char const*>(&read);
    failures += expect(
        std::all_of(scribbled, scribbled + sizeof(read), [](unsigned char byte) { return byte == 0xA5; }));
    ustore_executor_free(executor);
    return failures;
}

/** What a thread freeing an arena saw of the deferred call holding it once the free returned. */
struct arena_freeing_t {
    ustore_arena_t arena = nullptr;
    ustore_outcome_t* outcome = nullptr;
    ustore_status_t status_after_free = ustore_status_deferred_k;
    std::uint32_t freed = 0;
};

/** Frees the arena of @p freeing, then records the status of the call that held it. */
void free_arena(arena_freeing_t& freeing) noexcept {
    ustore_arena_free(freeing.arena);
    freeing.status_after_free = ustore_outcome_status(freeing.outcome);
    std::atomic_ref<std::uint32_t> {freeing.freed}.store(1, std::memory_order_release);
}

/** Freeing an arena waits until the deferred call holding it is final, here a read held behind an unfinished call. */
std::size_t arena_free_waits(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [executor, spawn_status] = spawn_executor(0);
    st::expected<executor_t> freeing_thread = executor_t::make(1, caller_exclusivity_t::caller_exclusive_k);
    failures += expect(spawn_status.succeeded() && freeing_thread.has_value());
    if (failures != 0) return failures;

    ustore_key_t const key = key_from_integer(600);
    failures += write_text(database, {&key, 1}, "value");
    ustore_arena_t arena = nullptr;
    ustore_outcome_t outcome {};
    ustore_arena_init_t init {.arena = &arena, .executor = executor, .outcome = &outcome};
    ustore_arena_init(&init);
    failures += expect_success(outcome);

    held_scan_t held;
    failures += submit_held(held, executor);
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, {&key, 1}, columns, arena, outcome);
    read.executor = executor;
    read.completion = ustore_completion_deferred_k;
    ustore_blobs_read(&read);

    arena_freeing_t freeing {.arena = arena, .outcome = &outcome};
    st::expected<fu_generation_t> const generation = freeing_thread->start<&free_arena>(freeing);
    failures += expect(generation.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    failures += expect(std::atomic_ref<std::uint32_t> {freeing.freed}.load(std::memory_order_acquire) == 0);

    failures += release_held(held);
    if (generation) freeing_thread->join(*generation);
    else free_arena(freeing);
    failures += expect(freeing.status_after_free == ustore_status_success_k);
    ustore_executor_free(executor);
    return failures;
}

/** A deferred call finding the deferred table full finishes contended at once, and a drain from inside a callback
 *  its own executor runs answers contended rather than waiting on itself. */
std::size_t executor_full_table(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [executor, spawn_status] = spawn_executor(1);
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;

    held_scan_t held;
    failures += submit_held(held, executor);
    ustore_key_t const key = key_from_integer(74);
    ustore_outcome_t outcome {};
    submit_writes(database, executor, {&key, 1}, {&outcome, 1});
    failures += expect_status(outcome, status_code_t::contended_k);
    failures += expect(held.drained == ustore_status_contended_k);
    failures += release_held(held);
    ustore_executor_free(executor);
    return failures;
}

/** A blocking call on an executor runs after every deferred call submitted before it, so it sees their writes. */
std::size_t executor_blocking_after_deferred(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [executor, spawn_status] = spawn_executor(0);
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());

    std::array<ustore_key_t, 3> const keys {key_from_integer(75), key_from_integer(76), key_from_integer(77)};
    ustore_outcome_t written {};
    ustore_blobs_write_t write {};
    write.database = database;
    write.executor = executor;
    write.outcome = &written;
    write.completion = ustore_completion_deferred_k;
    write.count = keys.size();
    write.keys = {keys.data(), sizeof(ustore_key_t)};
    write.values = stored_values_k.view();
    ustore_blobs_write(&write);

    read_columns_t columns;
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read = read_of(database, keys, columns, arena, outcome);
    read.executor = executor;
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(columns.presences && columns.presences[0] == 0b111);
    failures += expect_success(written);
    ustore_executor_free(executor);
    return failures;
}

/** A caller that sees a deferred call final may submit the next one at once, even to a table of one slot and naming the
 *  same arena, since the driver frees the table position and the claim before it finishes the outcome. */
std::size_t executor_submit_after_wait(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [executor, spawn_status] = spawn_executor(1);
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());

    ustore_key_t const key = key_from_integer(900);
    failures += write_text(database, {&key, 1}, "value");
    std::size_t contended = 0;
    std::size_t failed = 0;
    for (std::size_t round = 0; round != 1000; ++round) {
        read_columns_t columns;
        ustore_outcome_t outcome {};
        ustore_blobs_read_t read = read_of(database, {&key, 1}, columns, arena, outcome);
        read.executor = executor;
        read.completion = ustore_completion_deferred_k;
        ustore_blobs_read(&read);
        ustore_status_t const status = ustore_outcome_wait(&outcome);
        contended += status == ustore_status_contended_k;
        failed += status != ustore_status_success_k;
    }
    failures += expect_eq(contended, 0u);
    failures += expect_eq(failed, 0u);
    ustore_executor_free(executor);
    return failures;
}

/** A drain returns once every call submitted before it is final, without waiting on any of them. */
std::size_t executor_drain(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [executor, spawn_status] = spawn_executor(0);
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;

    std::array<ustore_key_t, 4> const keys {key_from_integer(78), key_from_integer(79), key_from_integer(80),
                                            key_from_integer(81)};
    std::array<ustore_outcome_t, 4> outcomes {};
    submit_writes(database, executor, keys, outcomes);

    ustore_outcome_t outcome {};
    ustore_executor_drain_t drain {.executor = executor, .outcome = &outcome, .mood = ustore_mood_spinning_k};
    ustore_executor_drain(&drain);
    failures += expect_success(outcome);
    for (ustore_outcome_t& written : outcomes) failures += expect_success(written);
    drain.mood = ustore_mood_parked_k;
    ustore_executor_drain(&drain);
    failures += expect_success(outcome);
    ustore_executor_free(executor);
    return failures;
}

/** A call naming an arena that an unfinished deferred call holds finishes contended, and succeeds once it is final. */
std::size_t executor_contended_arena(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [executor, spawn_status] = spawn_executor(0);
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());

    ustore_key_t const key = key_from_integer(82);
    failures += write_text(database, {&key, 1}, "value");
    held_scan_t held;
    failures += submit_held(held, executor);
    read_columns_t deferred_columns;
    ustore_outcome_t deferred_outcome {};
    ustore_blobs_read_t deferred_read = read_of(database, {&key, 1}, deferred_columns, arena, deferred_outcome);
    deferred_read.executor = executor;
    deferred_read.completion = ustore_completion_deferred_k;
    ustore_blobs_read(&deferred_read);

    read_columns_t columns;
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read = read_of(database, {&key, 1}, columns, arena, outcome);
    ustore_blobs_read(&read);
    failures += expect_status(outcome, status_code_t::contended_k);

    failures += release_held(held);
    failures += expect(ustore_outcome_wait(&deferred_outcome) == ustore_status_success_k);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(tape_written(columns.values) && columns.value(0) == "value");
    ustore_executor_free(executor);
    return failures;
}

/** A call on a transaction that an unfinished deferred call reads through finishes contended, and succeeds once it
 *  finishes. */
case_result_t executor_contended_transaction(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_transactions()) return case_result_t::skipped("the engine keeps no transactions", failures);
    auto [executor, spawn_status] = spawn_executor(0);
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());

    expected<transaction_t> transaction = database.transaction();
    failures += expect(transaction.has_value());
    if (failures != 0) return failures;
    held_scan_t held;
    failures += submit_held(held, executor);
    ustore_key_t const key = key_from_integer(83);
    read_columns_t columns;
    ustore_outcome_t deferred_outcome {};
    ustore_blobs_read_t deferred_read = read_of(database, {&key, 1}, columns, arena, deferred_outcome);
    deferred_read.executor = executor;
    deferred_read.completion = ustore_completion_deferred_k;
    deferred_read.view = ustore_transaction_view(transaction->transaction(), ustore_watch_keys_k);
    ustore_blobs_read(&deferred_read);

    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {};
    write.database = database;
    write.outcome = &outcome;
    write.transaction = transaction->transaction();
    write.count = 1;
    write.keys = {&key, 0};
    write.values = stored_values_k.view();
    ustore_blobs_write(&write);
    failures += expect_status(outcome, status_code_t::contended_k);

    failures += release_held(held);
    failures += expect(ustore_outcome_wait(&deferred_outcome) == ustore_status_success_k);
    ustore_blobs_write(&write);
    failures += expect_success(outcome);
    ustore_executor_free(executor);
    return failures;
}

/** Holds every thread of a lender's pool in one dispatch until released, so a borrower finds the pool running it. */
struct lender_hold_t {
    std::size_t entered = 0;
    std::uint32_t released = 0;
};

/** Holds one thread of the lender's pool until @p hold is released. */
void hold_lender_thread(lender_hold_t& hold) noexcept {
    std::atomic_ref<std::size_t> {hold.entered}.fetch_add(1, std::memory_order_acq_rel);
    std::atomic_ref<std::uint32_t> {hold.released}.wait(0, std::memory_order_acquire);
}

/** Tasks in every dispatch of a lender's own. */
constexpr std::size_t lender_tasks_k = 16;

/** A lender dispatching on its own pool over and over until the borrower's writes are final, and what it saw. */
struct lender_loop_t {
    executor_t& pool;
    std::size_t writes_expected = 0;
    std::size_t writes_finished = 0;
    std::size_t dispatched = 0;
    std::size_t refused = 0;
    std::size_t failed = 0;
    std::size_t tasks = 0;
};

/** Dispatches on the lender's pool until every write of @p lending is final, counting the dispatches the pool refused
 *  while it ran a borrower's batch. */
void lend_until_finished(lender_loop_t& lending) noexcept {
    std::atomic_ref<std::size_t> const finished {lending.writes_finished};
    do {
        st::status_t const status = lending.pool.for_n(lender_tasks_k, [&lending](std::size_t, std::size_t) noexcept {
            std::atomic_ref<std::size_t> {lending.tasks}.fetch_add(1, std::memory_order_relaxed);
        });
        if (status == st::status_t::operation_would_block_k) ++lending.refused;
        else if (st::failed(status)) ++lending.failed;
        else ++lending.dispatched;
    } while (finished.load(std::memory_order_acquire) != lending.writes_expected);
}

/** A borrowed pool finishes every deferred call while its lender's dispatch holds it, running them on the driver, and
 *  while its lender keeps dispatching on it from another thread; once the executor is freed, the lender dispatches
 *  alone. */
std::size_t executor_on_pool(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    st::expected<executor_t> lender = executor_t::make(2, caller_exclusivity_t::caller_exclusive_k);
    st::expected<executor_t> lending_thread = executor_t::make(1, caller_exclusivity_t::caller_exclusive_k);
    failures += expect(lender.has_value() && lending_thread.has_value());
    if (failures != 0) return failures;

    ustore_executor_t executor = nullptr;
    ustore_outcome_t outcome {};
    ustore_executor_on_pool_t lend {.executor = &executor, .outcome = &outcome, .pool = lender->view().pool()};
    ustore_executor_on_pool(&lend);
    failures += expect_success(outcome);
    if (failures != 0) return failures;

    // Both lender threads sit inside one dispatch, so every batch of the borrower is refused the pool
    lender_hold_t hold;
    st::expected<fu_generation_t> const holding = lender->start<&hold_lender_thread>(hold);
    failures += expect(holding.has_value());
    if (failures != 0) return failures;
    std::atomic_ref<std::size_t> const entered {hold.entered};
    failures += expect(spin_until([&entered]() noexcept { return entered.load(std::memory_order_acquire) == 2; }));
    std::array<ustore_key_t, 8> held_keys {};
    std::array<ustore_outcome_t, 8> held_outcomes {};
    for (std::size_t index = 0; index != held_keys.size(); ++index) held_keys[index] = key_from_integer(700 + index);
    submit_writes(database, executor, held_keys, held_outcomes);
    for (ustore_outcome_t& written : held_outcomes)
        failures += expect(ustore_outcome_wait(&written) == ustore_status_success_k);
    std::atomic_ref<std::uint32_t> const released {hold.released};
    released.store(1, std::memory_order_release);
    released.notify_all();
    lender->join(*holding);

    // The lender dispatches over and over from a thread of its own while the borrower's writes come and go
    constexpr std::size_t writes_k = 64;
    lender_loop_t lending {.pool = *lender, .writes_expected = writes_k};
    st::expected<fu_generation_t> const looping = lending_thread->start<&lend_until_finished>(lending);
    failures += expect(looping.has_value());
    if (failures != 0) return failures;
    std::array<ustore_key_t, writes_k> keys {};
    std::array<ustore_outcome_t, writes_k> outcomes {};
    for (std::size_t index = 0; index != writes_k; ++index) keys[index] = key_from_integer(100 + index);
    submit_writes(database, executor, keys, outcomes);
    for (ustore_outcome_t& written : outcomes) {
        failures += expect(ustore_outcome_wait(&written) == ustore_status_success_k);
        std::atomic_ref<std::size_t> {lending.writes_finished}.fetch_add(1, std::memory_order_release);
    }
    lending_thread->join(*looping);
    failures += expect(lending.failed == 0);
    failures += expect(lending.tasks == lending.dispatched * lender_tasks_k);

    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, keys, columns, arena, outcome);
    read.executor = executor;
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(columns.presences != nullptr);
    for (std::size_t octet = 0; columns.presences && octet != writes_k / 8; ++octet)
        failures += expect(columns.presences[octet] == 0xFF);
    ustore_executor_free(executor);

    std::size_t const tasks_before = lending.tasks;
    lending.writes_finished = writes_k;
    lend_until_finished(lending);
    failures += expect(lending.tasks == tasks_before + lender_tasks_k);
    return failures;
}

/** A call whose database or arena lives where its executor does not compute is refused with @c ustore_status_device_k.
 *  Host pages are the only residency a build makes, so the stand-in database and then the arena claim another. */
std::size_t executor_residency_mismatch(config_t const&) {
    std::size_t failures = 0;
    residency_t const elsewhere = static_cast<residency_t>(1);
    handle_t database {&progress_engine_k};
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;

    ustore_length_t const* offsets = nullptr;
    ustore_key_t const* keys = nullptr;
    ustore_outcome_t outcome {};
    ustore_blobs_scan_t scan {
        .database = c_handle<ustore_database_t>(&database),
        .outcome = &outcome,
        .count = 1,
        .offsets = &offsets,
        .keys = &keys,
        .arena = arena,
        .wanted = 1,
    };
    database.residency = elsewhere;
    ustore_blobs_scan(&scan);
    failures += expect_status(outcome, status_code_t::device_k);

    database.residency = residency_t::host_k;
    ustore_arena_t const placed = arena;
    placed->residency = elsewhere;
    ustore_blobs_scan(&scan);
    failures += expect_status(outcome, status_code_t::device_k);

    placed->residency = residency_t::host_k;
    ustore_blobs_scan(&scan);
    failures += expect_success(outcome);
    return failures;
}

/** Waiting on a deferred call blocks until its status is final, past the moment its callback returns. */
std::size_t outcome_wait_blocks(config_t const&) {
    std::size_t failures = 0;
    auto [executor, spawn_status] = spawn_executor(0);
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;

    held_scan_t held;
    failures += submit_held(held, executor);
    std::atomic_ref<hold_stage_t> const stage {held.stage};
    stage.store(hold_stage_t::released_k, std::memory_order_release);
    stage.notify_all();
    failures += expect(ustore_outcome_wait(&held.outcome) == ustore_status_success_k);
    failures += expect(stage.load(std::memory_order_acquire) == hold_stage_t::returned_k);
    ustore_executor_free(executor);
    return failures;
}

/** Freeing a database waits for the deferred call naming it, past the moment its callback returns. */
std::size_t database_free_waits(config_t const&) {
    std::size_t failures = 0;
    auto [executor, spawn_status] = spawn_executor(0);
    failures += expect(spawn_status.succeeded());
    if (failures != 0) return failures;

    held_scan_t held;
    failures += submit_held(held, executor);
    std::atomic_ref<hold_stage_t> const stage {held.stage};
    stage.store(hold_stage_t::released_k, std::memory_order_release);
    stage.notify_all();
    ustore_database_free(c_handle<ustore_database_t>(&held.database));
    failures += expect(ustore_outcome_status(&held.outcome) == ustore_status_success_k);
    failures += expect(stage.load(std::memory_order_acquire) == hold_stage_t::returned_k);
    ustore_executor_free(executor);
    return failures;
}

/** An init runs on an executor as every call does: a blocking one returns with the database open, and a deferred one
 *  submitted behind an unfinished call runs from its copy and writes the database before its outcome is final. */
case_result_t database_init_on_executor(config_t const& config) {
    if (config.engine() == "nvme") return case_result_t::skipped("the NVMe init borrows its executor until close");
    std::size_t failures = 0;
    auto [executor, spawn_status] = spawn_executor(0);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(spawn_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;

    ustore_database_t database = nullptr;
    ustore_outcome_t outcome {};
    open_engine(config.options(), &database, &outcome, executor, ustore_completion_blocking_k);
    failures += expect_success(outcome);
    failures += expect(database != nullptr);
    ustore_key_t const key = key_from_integer(1200);
    if (database) failures += write_text(database, {&key, 1}, "value");
    ustore_database_free(database);

    // The init waits behind the held scan, so it runs only after its struct died with the frame that built it
    held_scan_t held;
    failures += submit_held(held, executor);
    database = nullptr;
    open_engine(config.options(), &database, &outcome, executor, ustore_completion_deferred_k);
    failures += expect(ustore_outcome_status(&outcome) == ustore_status_deferred_k);
    failures += expect(database == nullptr);
    failures += release_held(held);
    failures += expect(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    failures += expect(database != nullptr);
    if (database) {
        read_columns_t columns;
        ustore_blobs_read_t read = read_of(database, {&key, 1}, columns, arena, outcome);
        ustore_blobs_read(&read);
        failures += expect_success(outcome);
        failures += expect(tape_written(columns.values) && columns.value(0) == "value");
    }
    ustore_database_free(database);
    ustore_executor_free(executor);
    return failures;
}

#pragma endregion Executors

/** A small read answers a tape of 32-bit offsets, @c count + 1 of them, the first at zero and the last closing the
 *  final value, beside lengths where a missing key reads as empty. */
std::size_t database_read_offsets(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());

    std::array<ustore_key_t, 3> const keys {key_from_integer(41), key_from_integer(42), key_from_integer(43)};
    failures += write_text(database, {&keys[0], 1}, "a");
    failures += write_text(database, {&keys[2], 1}, "bcd");
    read_columns_t columns;
    ustore_length_t const* lengths = nullptr;
    ustore_outcome_t outcome {};
    ustore_blobs_read_t read = read_of(database, keys, columns, arena, outcome);
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

/** A u32 tape, a u64 tape and an accessor holding the same values, one of them empty, write the same bytes. */
std::size_t database_write_layouts(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;

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
        write.keys = {keys.data(), sizeof(ustore_key_t)};
        write.values = layouts[layout];
        ustore_blobs_write(&write);
        failures += expect_success(outcome);

        read_columns_t columns;
        ustore_blobs_read_t read = read_of(database, keys, columns, arena, outcome);
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

/** The state of an accessor answering one value for every index, which records the largest block it was asked for. */
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

/** An accessor answering one value for every index broadcasts it to every key, asked in blocks of at most 64. */
std::size_t database_write_broadcast(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;

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
    write.keys = {keys.data(), sizeof(ustore_key_t)};
    write.values.layout = ustore_strings_accessor_k;
    write.values.data.accessor = {&get_repeated, &repeating};
    ustore_blobs_write(&write);
    failures += expect_success(outcome);
    failures += expect(largest_block != 0 && largest_block <= 64);

    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, keys, columns, arena, outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(tape_written(columns.values));
    if (!tape_written(columns.values)) return failures;
    std::size_t differing = 0;
    for (std::size_t index = 0; index != count; ++index) differing += columns.value(index) != "same";
    failures += expect_eq(differing, 0u);
    return failures;
}

/** A strings view in a layout no member of its union describes is refused with @c ustore_status_invalid_argument_k. */
std::size_t database_unknown_layout(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;

    ustore_key_t const key = key_from_integer(1700);
    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {};
    write.database = database;
    write.outcome = &outcome;
    write.count = 1;
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

    ustore_key_t const key = key_from_integer(1800);
    std::string_view const document = R"({"a":1})";
    strided_strings<std::string_view> const documents {{&document, 0}};
    ustore_outcome_t outcome {};
    ustore_docs_write_t write {
        .database = database,
        .outcome = &outcome,
        .count = 1,
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

/** A strings write takes its strings as a u64 tape and its values through an accessor, and a read naming the same
 *  strings by a u32 tape finds every value. */
case_result_t database_strings_layouts(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_collections())
        return case_result_t::skipped("the engine keeps no named collections", failures);
    auto [strings, strings_status] = create_collection(database, "layouts.strings", ustore_modality_strings_k);
    failures += expect(strings_status.succeeded());
    if (failures != 0) return failures;

    std::string_view const joined = "applebanana";
    std::array<std::uint64_t, 3> const wide_offsets {0, 5, 11};
    std::array<std::uint32_t, 3> const narrow_offsets {0, 5, 11};
    std::array<std::string_view, 2> const colors {"red", "yellow"};
    strided_strings<std::string_view> const accessed {strided(colors).begin()};
    ustore_outcome_t outcome {};
    ustore_strings_write_t write {
        .database = database,
        .outcome = &outcome,
        .count = colors.size(),
        .collections = {&strings, 0},
        .strings = u64_tape(joined.data(), wide_offsets.data()),
        .values = accessed.view(),
    };
    ustore_strings_write(&write);
    failures += expect_success(outcome);

    read_columns_t columns;
    ustore_strings_read_t read {
        .database = database,
        .outcome = &outcome,
        .count = colors.size(),
        .collections = {&strings, 0},
        .strings = u32_tape(joined.data(), narrow_offsets.data()),
        .presences = &columns.presences,
        .values = &columns.values,
        .arena = arena,
    };
    ustore_strings_read(&read);
    failures += expect_success(outcome);
    failures += expect(columns.presences && tape_written(columns.values));
    if (failures != 0) return failures;
    failures += expect_eq(columns.presences[0], 0b11);
    failures += expect_eq(columns.value(0), colors[0]);
    failures += expect_eq(columns.value(1), colors[1]);
    return failures;
}

/** A measurement without start or end keys spans the whole collection on every engine. */
std::size_t database_measure_whole(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());

    ustore_key_t const key = key_from_integer(950);
    failures += write_text(database, {&key, 1}, "value");
    ustore_size_t const* min_cardinalities = nullptr;
    ustore_size_t const* max_cardinalities = nullptr;
    ustore_outcome_t outcome {};
    ustore_blobs_measure_t measure {
        .database = database,
        .outcome = &outcome,
        .count = 1,
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
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());

    std::array<ustore_key_t, 3> const keys {key_from_integer(51), key_from_integer(52), key_from_integer(53)};
    failures += write_text(database, keys, "value");

    // The first and the last key go in one batch, which strides over the middle one
    ustore_outcome_t outcome {};
    ustore_blobs_remove_t remove {};
    remove.database = database;
    remove.outcome = &outcome;
    remove.count = 2;
    remove.keys = {keys.data(), 2 * sizeof(ustore_key_t)};
    ustore_blobs_remove(&remove);
    failures += expect_success(outcome);

    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, keys, columns, arena, outcome);
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
    if (!database.supports_transactions()) return case_result_t::skipped("the engine keeps no transactions", failures);
    expected<transaction_t> maybe_transaction = database.transaction();
    failures += expect(maybe_transaction.has_value());
    if (failures != 0) return failures;
    ustore_transaction_t const transaction = maybe_transaction->transaction();

    ustore_key_t const key = key_from_integer(61);
    failures += write_text(database, {&key, 1}, "staged", transaction);
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
    ustore_blobs_read_t read = read_of(database, {&key, 1}, columns, arena, outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(tape_written(columns.values) && columns.value(0) == "staged");
    return failures;
}

/** A stage reads at a stamp no later than the commit after it, and a snapshot taken next reads at that commit or past
 *  it. */
case_result_t database_stamps(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_transactions() || !database.supports_snapshots())
        return case_result_t::skipped("the engine keeps no transactions or no snapshots", failures);

    // A commit at the head first, so the stage reads above zero and a commit reporting nothing reads below it
    ustore_key_t const key = key_from_integer(62);
    failures += write_text(database, {&key, 1}, "head");
    expected<transaction_t> maybe_transaction = database.transaction();
    failures += expect(maybe_transaction.has_value());
    if (failures != 0) return failures;
    ustore_transaction_t const transaction = maybe_transaction->transaction();
    failures += write_text(database, {&key, 1}, "staged", transaction);

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

/** Freeing a database before its transaction and snapshot leaves both working, and freeing the last of them closes
 *  the database, so it opens again. */
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
    failures += write_text(database, {&key, 1}, "held", transaction);
    ustore_transaction_commit_t commit {.database = database, .outcome = &outcome, .transaction = transaction};
    ustore_transaction_commit(&commit);
    failures += expect_success(outcome);

    // The snapshot predates the commit, so it still misses the key
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, {&key, 1}, columns, arena, outcome);
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

/** A name resolves at the snapshot it is read at, so a collection created after the snapshot stays missing there. */
case_result_t database_find_at_snapshot(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_snapshots() || !database.supports_collections())
        return case_result_t::skipped("the engine keeps no snapshots of named collections", failures);

    expected<snapshot_t> snapshot = database.snapshot();
    failures += expect(snapshot.has_value());
    if (failures != 0) return failures;
    failures += expect(create_collection(database, "later").has_value());
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
    if (found) failures += expect(std::memcmp(found, &ustore_collection_main_k, sizeof(ustore_collection_id_t)) == 0);
    return failures;
}

/** Every status spelling reads back as its status, while a value or a name no status carries maps onto none. */
std::size_t status_names(config_t const&) {
    std::size_t failures = 0;
    auto const spelled = [](std::string_view name) noexcept {
        return ustore_bytes_view_t {reinterpret_cast<std::uint8_t const*>(name.data()), name.size()};
    };
    for (int status = ustore_status_success_k; status <= ustore_status_read_only_k; ++status)
        failures += expect(ustore_status_from_name(spelled(ustore_status_name(static_cast<ustore_status_t>(status)))) ==
                           status);
    failures += expect(std::string_view {ustore_status_name(ustore_status_deferred_k)} == "deferred");
    failures += expect(std::string_view {ustore_status_name(static_cast<ustore_status_t>(42))} == "unrecognized");
    failures += expect(ustore_status_from_name(spelled("unrecognized")) == ustore_status_invalid_argument_k);
    return failures;
}

/** Fails unless exactly @p count values below 64 have a spelling, each reading back as its value, while a name no
 *  enumerator carries reads back as the zero enumerator. */
template <typename enum_type_>
std::size_t expect_spellings(char const* (*name_of)(enum_type_), enum_type_ (*from_name)(ustore_bytes_view_t),
                             std::size_t count) {
    std::size_t failures = 0;
    std::size_t spelled = 0;
    // Values no enumerator carries reach the library as a C caller passes them, never loaded as the enum here
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

/** Every closed set spells each of its enumerators and reads every spelling back, and the version spells the numbers
 *  @c ustore_version reports. */
std::size_t closed_set_names(config_t const&) {
    std::size_t failures = 0;
    failures += expect_spellings(ustore_mood_name, ustore_mood_from_name, 2);
    failures += expect_spellings(ustore_durability_name, ustore_durability_from_name, 2);
    failures += expect_spellings(ustore_watch_name, ustore_watch_from_name, 2);
    failures += expect_spellings(ustore_scan_mode_name, ustore_scan_mode_from_name, 2);
    failures += expect_spellings(ustore_drop_mode_name, ustore_drop_mode_from_name, 3);
    failures += expect_spellings(ustore_isolation_name, ustore_isolation_from_name, 2);
    failures += expect_spellings(ustore_transport_name, ustore_transport_from_name, 5);
    failures += expect_spellings(ustore_engine_name, ustore_engine_from_name, 4);
    failures += expect_spellings(ustore_modality_name, ustore_modality_from_name, 6);
    failures += expect_spellings(ustore_vertex_role_name, ustore_vertex_role_from_name, 4);
    failures += expect_spellings(ustore_docs_field_type_name, ustore_docs_field_type_from_name, 17);
    failures += expect_spellings(ustore_docs_modification_name, ustore_docs_modification_from_name, 5);
    failures += expect_spellings(ustore_page_encoding_name, ustore_page_encoding_from_name, 5);
    failures += expect(std::string_view {ustore_isolation_name(ustore_isolation_strict_serializable_k)} ==
                       "strict-serializable");
    int major = 0, minor = 0, patch = 0;
    ustore_version(&major, &minor, &patch);
    failures += expect(fmt::format("{}.{}.{}", major, minor, patch) == ustore_version_name());
    return failures;
}

/** A description names the engine that opened the database, and the RAM engine its isolation and its keys, while a
 *  compaction keeps every key. */
std::size_t database_describe_compact(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_raw(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    std::array<ustore_key_t, 3> const keys {key_from_integer(31), key_from_integer(32), key_from_integer(33)};
    failures += write_text(database, keys, "kept");

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
    ustore_blobs_read_t read = read_of(database, keys, columns, arena, outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(columns.presences && columns.presences[0] == 7 && columns.value(2) == "kept");
    ustore_database_free(database);
    return failures;
}

/** A clear empties every value keeping the keys, then removes every pair keeping the collections, then drops every
 *  collection but the main one, as each mode asks. */
std::size_t database_clear_modes(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_raw(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(create_collection(database, "clear.kept").has_value());
    ustore_key_t const key = key_from_integer(77);
    failures += write_text(database, {&key, 1}, "value");

    ustore_outcome_t outcome {};
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, {&key, 1}, columns, arena, outcome);
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

    clear.mode = ustore_drop_collection_k;
    ustore_database_clear(&clear);
    failures += expect_success(outcome);
    failures += expect_status(collection_named(receiver_t {database}, "clear.kept"), status_code_t::not_found_k);
    ustore_database_free(database);
    return failures;
}

/** A database RocksDB opens read-only serves reads, snapshots and their saves, and refuses every kind of call that
 *  would change it with @c ustore_status_read_only_k. */
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
    failures += write_text(database, {&key, 1}, "viewed");
    auto [created, create_status] = create_collection(database, "view.kept");
    failures += expect(create_status.succeeded());
    ustore_database_free(database);

    ustore_database_view_rocksdb_t view {.database = &database, .outcome = &outcome, .directory = path};
    ustore_database_view_rocksdb(&view);
    failures += expect_success(outcome);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(arena_status.succeeded());
    if (failures != 0) return failures;
    read_columns_t columns;
    ustore_blobs_read_t read = read_of(database, {&key, 1}, columns, arena, outcome);
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
                                .keys = {&key, 0},
                                .values = stored_values_k.view()};
    ustore_blobs_write(&write);
    failures += expect_status(status_t {outcome}, status_code_t::read_only_k);
    ustore_blobs_remove_t remove {.database = database, .outcome = &outcome, .count = 1, .keys = {&key, 0}};
    ustore_blobs_remove(&remove);
    failures += expect_status(status_t {outcome}, status_code_t::read_only_k);
    ustore_transaction_t transaction = nullptr;
    ustore_transaction_init_t begin {.transaction = &transaction, .database = database, .outcome = &outcome};
    ustore_transaction_init(&begin);
    failures += expect_status(status_t {outcome}, status_code_t::read_only_k);
    failures += expect(transaction == nullptr);
    failures += expect_status(create_collection(database, "view.created"), status_code_t::read_only_k);
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

#pragma region Pages

/** Counts a failure unless @p outcome carries @p wanted, then resets it for the next call. */
std::size_t refused(ustore_outcome_t& outcome, status_code_t wanted) {
    std::size_t const failures = expect_status(status_t {outcome}, wanted);
    outcome = {};
    return failures;
}

/** The pages calls refuse what their header refuses, writing no outputs: missing keys and elements, unknown
 *  encodings, HTML, Markdown and JSON as unsupported, markup that is not well-formed XML, and edits of a missing page,
 *  while a read asking only for presences takes any encoding. */
case_result_t pages_refusals(config_t const& config) {
    std::size_t failures = 0;
    auto [owner, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    failures += expect(owner.clear(ustore_drop_collection_k).succeeded());
    ustore_database_t const database = owner;

    ustore_key_t const page = key_from_integer(901);
    ustore_key_t const missing = key_from_integer(902);
    // C++ may not form an enum value no enumerator names, even by `std::bit_cast`, so the integer lands as bytes
    using encoding_bits_t = std::underlying_type_t<ustore_page_encoding_t>;
    auto import = [&](std::string_view markup, encoding_bits_t encoding, ustore_key_t const* key) {
        strided_strings<std::string_view> const values {{&markup, 0}};
        ustore_outcome_t outcome {};
        ustore_pages_import_t import {};
        import.database = database, import.outcome = &outcome, import.count = 1;
        import.keys = {key, 0}, import.values = values.view();
        std::memcpy(reinterpret_cast<std::byte*>(&import.encoding), &encoding, sizeof(encoding));
        ustore_pages_import(&import);
        return status_t {outcome};
    };
    // A probe through an encoding every engine serving pages takes tells the NVMe engine apart, which serves none yet
    if (import("<a/>", ustore_page_encoding_xml_k, &page).code == status_code_t::unsupported_k)
        return case_result_t::skipped("the engine serves no pages yet", failures);

    failures += expect_status(import("<a/>", ustore_page_encoding_html_k, &page), status_code_t::unsupported_k);
    failures += expect_status(import("# a", ustore_page_encoding_markdown_k, &page), status_code_t::unsupported_k);
    failures += expect_status(import("{}", ustore_page_encoding_json_k, &page), status_code_t::unsupported_k);
    failures += expect_status(import("<a/>", 42, &page), status_code_t::invalid_argument_k);
    failures += expect_status(import("<a/>", ustore_page_encoding_xml_k, nullptr), status_code_t::invalid_argument_k);
    for (std::string_view malformed : {"<a>", "<a>&nbsp;</a>", "<a b=\"<\"/>", "<a>&#0;</a>", "a & b"})
        failures += expect_status(import(malformed, ustore_page_encoding_xml_k, &page), status_code_t::malformed_k);
    failures += expect_status(import("a & b", ustore_page_encoding_text_k, &page), status_code_t::success_k);

    // An insertion refused anywhere leaves its effective keys unwritten
    std::string_view const fragment = "<b/>";
    strided_strings<std::string_view> const fragments {{&fragment, 0}};
    ustore_key_t const* effective = nullptr;
    ustore_outcome_t outcome {};
    ustore_pages_insert_t insert {};
    insert.database = database, insert.outcome = &outcome, insert.count = 1, insert.keys = {&missing, 0};
    insert.values = fragments.view(), insert.encoding = ustore_page_encoding_xml_k;
    insert.effective_keys = &effective, insert.arena = arena;
    ustore_pages_insert(&insert);
    failures += refused(outcome, status_code_t::not_found_k);
    failures += expect(effective == nullptr);
    insert.keys = {&page, 0};
    insert.values = {};
    ustore_pages_insert(&insert);
    failures += refused(outcome, status_code_t::invalid_argument_k);
    insert.values = fragments.view();
    ustore_pages_insert(&insert);
    failures += expect_success(outcome);
    failures += expect(effective != nullptr);

    ustore_pages_remove_t remove {};
    remove.database = database, remove.outcome = &outcome, remove.count = 1, remove.keys = {&page, 0};
    ustore_pages_remove(&remove);
    failures += refused(outcome, status_code_t::invalid_argument_k);

    ustore_key_t const element = page;
    ustore_pages_splice_t splice {};
    splice.database = database, splice.outcome = &outcome, splice.count = 1, splice.keys = {&missing, 0};
    splice.elements = {&element, 0};
    ustore_pages_splice(&splice);
    failures += refused(outcome, status_code_t::not_found_k);

    ustore_octet_t const* presences = nullptr;
    ustore_strings_view_t values {};
    ustore_pages_read_t read {};
    read.database = database, read.outcome = &outcome, read.count = 1, read.keys = {&page, 0};
    read.presences = &presences, read.values = &values, read.arena = arena;
    read.outputs_encoding = ustore_page_encoding_html_k;
    ustore_pages_read(&read);
    failures += refused(outcome, status_code_t::unsupported_k);
    failures += expect(presences == nullptr && !tape_written(values));
    read.values = nullptr;
    ustore_pages_read(&read);
    failures += expect_success(outcome);
    failures += expect(presences != nullptr && (presences[0] & 1));
    return failures;
}

#pragma endregion Pages

#pragma region Cursors

/** Writes @p text under every key of @p keys in @p collection at the head. */
std::size_t write_into(ustore_database_t database, ustore_collection_id_t collection,
                       std::span<ustore_key_t const> keys, std::string_view text) {
    strided_strings<std::string_view> const repeated {{&text, 0}};
    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {.database = database,
                                .outcome = &outcome,
                                .count = keys.size(),
                                .collections = {&collection, 0},
                                .keys = {keys.data(), sizeof(ustore_key_t)},
                                .values = repeated.view()};
    ustore_blobs_write(&write);
    return expect_success(outcome);
}

/** The columns one pull of a blobs cursor writes into its arena, each null until the pull reports it. */
struct pulled_keys_t {
    ustore_key_t const* keys = nullptr;
    ustore_strings_view_t values {};
    ustore_size_t written = 0;

    /** The value of key @p index. */
    std::string_view value(std::size_t index) const noexcept { return string_of(tape_string(values, index)); }
};

/** A blocking pull of up to @p wanted keys and their values from @p cursor into @p pulled inside @p arena. */
ustore_blobs_cursor_next_t pull_of(ustore_database_t database, ustore_blobs_cursor_t cursor, pulled_keys_t& pulled,
                                   ustore_arena_t arena, ustore_size_t wanted, ustore_outcome_t& outcome) noexcept {
    return {.database = database,
            .outcome = &outcome,
            .cursor = cursor,
            .keys = &pulled.keys,
            .values = &pulled.values,
            .written = &pulled.written,
            .arena = arena,
            .wanted = wanted};
}

/** Opens a cursor over @p collection of @p database from its first key, at what @p view sees. */
expected<ustore_blobs_cursor_t> open_cursor(ustore_database_t database, ustore_collection_id_t collection,
                                            ustore_view_t view = nullptr) noexcept {
    ustore_blobs_cursor_t cursor = nullptr;
    ustore_outcome_t outcome {};
    ustore_blobs_cursor_init_t init {.cursor = &cursor,
                                     .database = database,
                                     .outcome = &outcome,
                                     .view = view,
                                     .collection = collection};
    ustore_blobs_cursor_init(&init);
    if (outcome.status != ustore_status_success_k) return status_t {outcome};
    return cursor;
}

/** Counts one failure for every key of the page @p pulled that is not @p wanted in order, or reads another value. */
std::size_t expect_page(pulled_keys_t const& pulled, std::span<ustore_key_t const> wanted, std::string_view value) {
    std::size_t failures = expect_eq(pulled.written, wanted.size());
    if (pulled.written != wanted.size()) return failures;
    for (std::size_t index = 0; index != wanted.size(); ++index) {
        failures += expect_eq(pulled.keys[index], wanted[index]);
        failures += expect_eq(pulled.value(index), value);
    }
    return failures;
}

/** A blobs cursor walks a collection whole in pages with the values of its keys, and a strings cursor walks one prefix
 *  whole, each pulling nothing once a page came back short. */
case_result_t cursor_whole_walk(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_collections())
        return case_result_t::skipped("the engine keeps no named collections", failures);
    auto [blobs, blobs_status] = create_collection(database, "cursor.walk");
    auto [strings, strings_status] = create_collection(database, "cursor.walk.strings", ustore_modality_strings_k);
    failures += expect(blobs_status.succeeded() && strings_status.succeeded());
    if (failures != 0) return failures;

    std::array<ustore_key_t, 10> keys;
    for (std::size_t index = 0; index != keys.size(); ++index) keys[index] = key_from_integer(100 + index);
    failures += write_into(database, blobs, keys, "walked");
    auto [cursor, cursor_status] = open_cursor(database, blobs);
    failures += expect(cursor_status.succeeded());
    if (failures != 0) return failures;
    ustore_outcome_t outcome {};
    for (std::size_t first : {0u, 4u, 8u, 10u, 10u}) {
        pulled_keys_t pulled;
        ustore_blobs_cursor_next_t next = pull_of(database, cursor, pulled, arena, 4, outcome);
        ustore_blobs_cursor_next(&next);
        failures += expect_success(outcome);
        std::span<ustore_key_t const> const wanted {keys.data() + first, std::min<std::size_t>(4, 10 - first)};
        failures += expect_page(pulled, wanted, "walked");
    }
    ustore_blobs_cursor_free(cursor);

    std::array<std::string_view, 4> const texts {"b:1", "a:3", "a:1", "a:2"};
    strided_strings<std::string_view> const accessed {strided(texts).begin()};
    ustore_strings_write_t write {.database = database,
                                  .outcome = &outcome,
                                  .count = texts.size(),
                                  .collections = {&strings, 0},
                                  .strings = accessed.view(),
                                  .values = accessed.view()};
    ustore_strings_write(&write);
    failures += expect_success(outcome);
    ustore_strings_cursor_t walk = nullptr;
    ustore_strings_cursor_init_t init {.cursor = &walk,
                                       .database = database,
                                       .outcome = &outcome,
                                       .collection = strings,
                                       .prefix = bytes_of("a:")};
    ustore_strings_cursor_init(&init);
    failures += expect_success(outcome);
    std::array<std::string_view, 3> const prefixed {"a:1", "a:2", "a:3"};
    for (std::size_t first : {0u, 2u, 3u}) {
        ustore_strings_view_t found {};
        ustore_strings_view_t values {};
        ustore_size_t written = 0;
        ustore_strings_cursor_next_t next {.database = database,
                                           .outcome = &outcome,
                                           .cursor = walk,
                                           .strings = &found,
                                           .values = &values,
                                           .written = &written,
                                           .arena = arena,
                                           .wanted = 2};
        ustore_strings_cursor_next(&next);
        failures += expect_success(outcome);
        failures += expect_eq(written, std::min<std::size_t>(2, 3 - first));
        for (std::size_t index = 0; index != written && first + index != prefixed.size(); ++index) {
            failures += expect_eq(string_of(tape_string(found, index)), prefixed[first + index]);
            failures += expect_eq(string_of(tape_string(values, index)), prefixed[first + index]);
        }
    }
    ustore_strings_cursor_free(walk);
    return failures;
}

/** A cursor keeps only the key its next pull starts at, so keys written behind it stay behind and keys written ahead
 *  of it come, until a short page exhausts it. */
case_result_t cursor_resume_after_writes(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_collections())
        return case_result_t::skipped("the engine keeps no named collections", failures);
    auto [collection, create_status] = create_collection(database, "cursor.resume");
    failures += expect(create_status.succeeded());
    if (failures != 0) return failures;

    std::array<ustore_key_t, 4> const written {key_from_integer(10), key_from_integer(20), key_from_integer(30),
                                               key_from_integer(40)};
    failures += write_into(database, collection, written, "first");
    auto [cursor, cursor_status] = open_cursor(database, collection);
    failures += expect(cursor_status.succeeded());
    if (failures != 0) return failures;
    ustore_outcome_t outcome {};
    pulled_keys_t pulled;
    ustore_blobs_cursor_next_t next = pull_of(database, cursor, pulled, arena, 2, outcome);
    ustore_blobs_cursor_next(&next);
    failures += expect_success(outcome);
    failures += expect_page(pulled, {written.data(), 2}, "first");

    std::array<ustore_key_t, 2> const later {key_from_integer(15), key_from_integer(35)};
    failures += write_into(database, collection, later, "later");
    next.wanted = 8;
    ustore_blobs_cursor_next(&next);
    failures += expect_success(outcome);
    failures += expect_eq(pulled.written, 3u);
    if (pulled.written == 3) {
        failures += expect_eq(pulled.keys[0], key_from_integer(30));
        failures += expect_eq(pulled.keys[1], key_from_integer(35));
        failures += expect_eq(pulled.value(1), "later");
        failures += expect_eq(pulled.keys[2], key_from_integer(40));
    }

    ustore_key_t const beyond = key_from_integer(50);
    failures += write_into(database, collection, {&beyond, 1}, "beyond");
    ustore_blobs_cursor_next(&next);
    failures += expect_success(outcome);
    failures += expect_eq(pulled.written, 0u);
    failures += expect(pulled.keys == nullptr && !tape_written(pulled.values));
    ustore_blobs_cursor_free(cursor);
    return failures;
}

/** A cursor through a snapshot walks what the snapshot saw, and holds the snapshot after its holder frees it. */
case_result_t cursor_snapshot_view(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_collections() || !database.supports_snapshots())
        return case_result_t::skipped("the engine keeps no named collections or snapshots", failures);
    auto [collection, create_status] = create_collection(database, "cursor.snapshot");
    failures += expect(create_status.succeeded());
    if (failures != 0) return failures;

    std::array<ustore_key_t, 3> const seen {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
    failures += write_into(database, collection, seen, "seen");
    ustore_outcome_t outcome {};
    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t snapshot_init {.snapshot = &snapshot, .database = database, .outcome = &outcome};
    ustore_snapshot_init(&snapshot_init);
    failures += expect_success(outcome);
    ustore_key_t const unseen = key_from_integer(4);
    failures += write_into(database, collection, {&unseen, 1}, "unseen");
    ustore_blobs_remove_t remove {.database = database,
                                  .outcome = &outcome,
                                  .count = 1,
                                  .collections = {&collection, 0},
                                  .keys = {&seen[1], 0}};
    ustore_blobs_remove(&remove);
    failures += expect_success(outcome);

    auto [cursor, cursor_status] = open_cursor(database, collection, ustore_snapshot_view(snapshot));
    failures += expect(cursor_status.succeeded());
    ustore_snapshot_free(snapshot);
    if (failures != 0) return failures;
    pulled_keys_t pulled;
    ustore_blobs_cursor_next_t next = pull_of(database, cursor, pulled, arena, 8, outcome);
    ustore_blobs_cursor_next(&next);
    failures += expect_success(outcome);
    failures += expect_page(pulled, seen, "seen");
    ustore_blobs_cursor_free(cursor);
    return failures;
}

/** What a thread freeing a cursor saw of the deferred pull naming it once the free returned. */
struct cursor_freeing_t {
    ustore_blobs_cursor_t cursor = nullptr;
    ustore_outcome_t* outcome = nullptr;
    ustore_status_t status_after_free = ustore_status_deferred_k;
    std::uint32_t freed = 0;
};

/** Frees the cursor of @p freeing, then records the status of the pull that named it. */
void free_cursor(cursor_freeing_t& freeing) noexcept {
    ustore_blobs_cursor_free(freeing.cursor);
    freeing.status_after_free = ustore_outcome_status(freeing.outcome);
    std::atomic_ref<std::uint32_t> {freeing.freed}.store(1, std::memory_order_release);
}

/** Freeing a cursor waits until the deferred pull naming it is final, here a pull held behind an unfinished call. */
case_result_t cursor_free_waits(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_collections())
        return case_result_t::skipped("the engine keeps no named collections", failures);
    auto [executor, spawn_status] = spawn_executor(0);
    st::expected<executor_t> freeing_thread = executor_t::make(1, caller_exclusivity_t::caller_exclusive_k);
    auto [collection, create_status] = create_collection(database, "cursor.free");
    failures += expect(spawn_status.succeeded() && freeing_thread.has_value() && create_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make(executor);
    std::array<ustore_key_t, 2> const keys {key_from_integer(1), key_from_integer(2)};
    failures += write_into(database, collection, keys, "held");
    auto [cursor, cursor_status] = open_cursor(database, collection);
    failures += expect(arena_status.succeeded() && cursor_status.succeeded());
    if (failures != 0) return failures;

    held_scan_t held;
    failures += submit_held(held, executor);
    ustore_outcome_t outcome {};
    pulled_keys_t pulled;
    ustore_blobs_cursor_next_t next = pull_of(database, cursor, pulled, arena, 8, outcome);
    next.executor = executor;
    next.completion = ustore_completion_deferred_k;
    ustore_blobs_cursor_next(&next);

    cursor_freeing_t freeing {.cursor = cursor, .outcome = &outcome};
    st::expected<fu_generation_t> const generation = freeing_thread->start<&free_cursor>(freeing);
    failures += expect(generation.has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    failures += expect(std::atomic_ref<std::uint32_t> {freeing.freed}.load(std::memory_order_acquire) == 0);

    failures += release_held(held);
    if (generation) freeing_thread->join(*generation);
    else free_cursor(freeing);
    failures += expect(freeing.status_after_free == ustore_status_success_k);
    failures += expect_page(pulled, keys, "held");
    ustore_executor_free(executor);
    return failures;
}

#pragma endregion Cursors

#pragma region Placement

/** A RAM executor refuses a memory domain the machine lacks, leaving its output null, as does one borrowing a pool,
 *  and keeps every arena it makes on the first domain it serves. */
std::size_t executor_memory_domains(config_t const&) {
    std::size_t failures = 0;
    st::expected<topology_t> topology = topology_t::make();
    st::expected<executor_t> lender = executor_t::make(1, caller_exclusivity_t::caller_exclusive_k);
    failures += expect(topology.has_value() && lender.has_value());
    if (failures != 0) return failures;
    std::int32_t const known = topology->memory_domain(0);
    std::int32_t const unknown = 1 << 20;

    ustore_outcome_t outcome {};
    ustore_executor_t executor = nullptr;
    ustore_executor_for_ram_t refused {.executor = &executor, .outcome = &outcome, .ram = {&unknown, 1}};
    ustore_executor_for_ram(&refused);
    failures += expect_status(status_t {outcome}, status_code_t::invalid_argument_k);
    failures += expect(executor == nullptr);
    ustore_executor_on_pool_t lent {.executor = &executor,
                                    .outcome = &outcome,
                                    .ram = {&unknown, 1},
                                    .pool = lender->view().pool()};
    ustore_executor_on_pool(&lent);
    failures += expect_status(status_t {outcome}, status_code_t::invalid_argument_k);
    failures += expect(executor == nullptr);

    ustore_executor_for_ram_t placed {.executor = &executor, .outcome = &outcome, .ram = {&known, 1}, .threads = 1};
    ustore_executor_for_ram(&placed);
    failures += expect_success(outcome);
    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());
    if (arena_status.succeeded()) failures += expect_eq(static_cast<ustore_arena_t>(arena)->memory_domain, known);
    ustore_executor_free(executor);

    // Zero threads take 7/8 of the cores of every domain the process may allocate on
    ustore_executor_for_ram_t derived {.executor = &executor, .outcome = &outcome};
    ustore_executor_for_ram(&derived);
    failures += expect_success(outcome);
    ustore_executor_free(executor);
    return failures;
}

#pragma endregion Placement

#pragma region Progress

/** What a progress callback saw of the call it watches, kept in plain fields, so a sanitizer flags two callbacks of
 *  one call running at once. */
struct progress_record_t {

    /** The outcome of a deferred call, which must read deferred while its callback runs, or null for a blocking one. */
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

/** Records one report into the @c progress_record_t behind @p state, cancelling at its @c cancelling_report. */
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

/** A scan of three tasks whose callback cancels after the first finishes cancelled and leaves its outputs null, for
 *  keys and strings alike. */
case_result_t progress_cancel_scans(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    std::array<ustore_key_t, 3> const keys {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
    failures += write_text(database, keys, "scanned");

    ustore_outcome_t outcome {};
    progress_record_t record {.cancelling_report = 1};
    ustore_length_t const* offsets = nullptr;
    ustore_key_t const* found = nullptr;
    ustore_blobs_scan_t scan {.database = database,
                              .outcome = &outcome,
                              .count = 3,
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

    if (!database.supports_collections())
        return case_result_t::skipped("the engine keeps no named collections", failures);
    auto [strings, strings_status] = create_collection(database, "progress.strings", ustore_modality_strings_k);
    failures += expect(strings_status.succeeded());
    if (failures != 0) return failures;
    std::array<std::string_view, 2> const prefixes {"a", "b"};
    strided_strings<std::string_view> const accessed {strided(prefixes).begin()};
    ustore_strings_write_t write {.database = database,
                                  .outcome = &outcome,
                                  .count = prefixes.size(),
                                  .collections = {&strings, 0},
                                  .strings = accessed.view(),
                                  .values = accessed.view()};
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

/** A pull its callback cancels publishes nothing and leaves the cursor where it was, so the next pull repeats it. */
case_result_t progress_cancel_cursor(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_collections())
        return case_result_t::skipped("the engine keeps no named collections", failures);
    auto [collection, create_status] = create_collection(database, "progress.cursor");
    failures += expect(create_status.succeeded());
    std::array<ustore_key_t, 3> const keys {key_from_integer(1), key_from_integer(2), key_from_integer(3)};
    failures += write_into(database, collection, keys, "pulled");
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

/** Whether the saved copy at @p path is gone: no image in the directory the RAM engine saves into, and no database
 *  where RocksDB and LevelDB create one. */
bool nothing_saved(config_t const& config, char const* path) noexcept {
    std::error_code error;
    if (config.engine() == "ram")
        return !std::filesystem::exists(path_t {path, "ram.image"}.c_str(), error) &&
               !std::filesystem::exists(path_t {path, "ram.image.partial"}.c_str(), error);
    return !std::filesystem::exists(path, error) || std::filesystem::is_empty(path, error);
}

/** Readies @p path for a save: a fresh directory the RAM engine saves into, or no path where the others create it. */
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

/** A save its callback cancels at the first report removes what it wrote, so a retry at the same path succeeds. */
case_result_t progress_cancel_save(config_t const& config) {
    if (!config.directory()) return case_result_t::skipped("a save needs a directory");
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    failures += expect(open_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_snapshots()) return case_result_t::skipped("the engine keeps no snapshots", failures);
    ustore_key_t const key = key_from_integer(9);
    failures += write_text(database, {&key, 1}, "saved");
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
    ustore_snapshot_save(&save);
    failures += expect_success(outcome);
    ustore_snapshot_free(snapshot);
    auto [copy, copy_status] = open_saved(config, saved.c_str());
    failures += expect(copy_status.succeeded());
    ustore_database_free(copy);
    return failures;
}

/** A compaction its callback cancels at the first report stops between collections, and the data stays readable. */
case_result_t progress_cancel_compact(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_collections())
        return case_result_t::skipped("the engine keeps no named collections", failures);
    failures += expect(create_collection(database, "progress.compacted").has_value());
    ustore_key_t const key = key_from_integer(11);
    failures += write_text(database, {&key, 1}, "compacted");

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
    ustore_blobs_read_t read = read_of(database, {&key, 1}, columns, arena, outcome);
    ustore_blobs_read(&read);
    failures += expect_success(outcome);
    failures += expect(tape_written(columns.values) && columns.value(0) == "compacted");
    return failures;
}

/** The callbacks of a deferred scan, pull, compaction and save each run one at a time, before their call is final, with
 *  counts that never decrease and end at the total. The record counts without atomics, so ThreadSanitizer flags two
 *  callbacks of one call running at once. */
std::size_t progress_serialized(config_t const& config) {
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [executor, spawn_status] = spawn_executor(0);
    failures += expect(open_status.succeeded() && spawn_status.succeeded());
    if (failures != 0) return failures;
    auto [arena, arena_status] = arena_t::make(executor);
    failures += expect(arena_status.succeeded());
    std::array<ustore_key_t, 4> const keys {key_from_integer(1), key_from_integer(2), key_from_integer(3),
                                            key_from_integer(4)};
    failures += write_text(database, keys, "reported");
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
    auto [cursor, cursor_status] = open_cursor(database, ustore_collection_main_k);
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

/** A save copies exactly the collections and names its snapshot recorded: one created after the snapshot stays
 *  behind, and one dropped after it comes along with its pairs. */
case_result_t snapshot_save_recorded(config_t const& config) {
    if (!config.directory()) return case_result_t::skipped("a save needs a directory");
    std::size_t failures = 0;
    auto [database, open_status] = open_database<database_t>(config);
    auto [arena, arena_status] = arena_t::make();
    failures += expect(open_status.succeeded() && arena_status.succeeded());
    if (failures != 0) return failures;
    if (!database.supports_collections() || !database.supports_snapshots())
        return case_result_t::skipped("the engine keeps no named collections or snapshots", failures);
    auto [dropped, dropped_status] = create_collection(database, "save.dropped");
    failures += expect(dropped_status.succeeded());
    ustore_key_t const key = key_from_integer(21);
    failures += write_into(database, dropped, {&key, 1}, "recorded");
    if (failures != 0) return failures;

    ustore_outcome_t outcome {};
    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t snapshot_init {.snapshot = &snapshot, .database = database, .outcome = &outcome};
    ustore_snapshot_init(&snapshot_init);
    failures += expect_success(outcome);
    failures += expect(create_collection(database, "save.created").has_value());
    ustore_collection_drop_t drop {.database = database, .outcome = &outcome, .collection = dropped};
    ustore_collection_drop(&drop);
    failures += expect_success(outcome);

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
    ustore_blobs_read_t read = read_of(copy, {&key, 1}, columns, arena, outcome);
    read.collections = {&dropped, 0};
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
    report(totals, "pages.refusals", pages_refusals, config);
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
    report(totals, "database.strings_layouts", database_strings_layouts, config);
    report(totals, "database.measure_whole", database_measure_whole, config);
    report(totals, "database.remove", database_remove, config);
    report(totals, "database.rollback", database_rollback, config);
    report(totals, "database.stamps", database_stamps, config);
    report(totals, "database.free_holds", database_free_holds, config);
    report(totals, "database.find_at_snapshot", database_find_at_snapshot, config);
    report(totals, "database.describe_compact", database_describe_compact, config);
    report(totals, "database.clear_modes", database_clear_modes, config);
    report(totals, "database.view_refusals", database_view_refusals, config);
    report(totals, "arena.null_refused", arena_null_refused, config);
    report(totals, "arena.reuse", arena_reuse, config);
    report(totals, "arena.failed_outputs_null", arena_failed_outputs_null, config);
    report(totals, "arena.aligned_outputs", arena_aligned_outputs, config);
    report(totals, "arena.outputs_only", arena_outputs_only, config);
    report(totals, "arena.free_waits", arena_free_waits, config);
    report(totals, "executor.deferred_round_trip", executor_deferred_round_trip, config);
    report(totals, "executor.copied_submission", executor_copied_submission, config);
    report(totals, "executor.full_table", executor_full_table, config);
    report(totals, "executor.blocking_after_deferred", executor_blocking_after_deferred, config);
    report(totals, "executor.submit_after_wait", executor_submit_after_wait, config);
    report(totals, "executor.drain", executor_drain, config);
    report(totals, "executor.contended_arena", executor_contended_arena, config);
    report(totals, "executor.contended_transaction", executor_contended_transaction, config);
    report(totals, "executor.on_pool", executor_on_pool, config);
    report(totals, "executor.residency_mismatch", executor_residency_mismatch, config);
    report(totals, "outcome.wait_blocks", outcome_wait_blocks, config);
    report(totals, "database.free_waits", database_free_waits, config);
    report(totals, "database.init_on_executor", database_init_on_executor, config);
    report(totals, "cursor.whole_walk", cursor_whole_walk, config);
    report(totals, "cursor.resume_after_writes", cursor_resume_after_writes, config);
    report(totals, "cursor.snapshot_view", cursor_snapshot_view, config);
    report(totals, "cursor.free_waits", cursor_free_waits, config);
    report(totals, "executor.memory_domains", executor_memory_domains, config);
    report(totals, "progress.cancel_scans", progress_cancel_scans, config);
    report(totals, "progress.cancel_cursor", progress_cancel_cursor, config);
    report(totals, "progress.cancel_save", progress_cancel_save, config);
    report(totals, "progress.cancel_compact", progress_cancel_compact, config);
    report(totals, "progress.serialized", progress_serialized, config);
    report(totals, "snapshot.save_recorded", snapshot_save_recorded, config);
    return suite_exit_code(totals);
}
