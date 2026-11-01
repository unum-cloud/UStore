/**
 *  @file test/abi/fixture.hpp
 *  @author Ash Vardanian
 *  @date November 1, 2026
 *  @brief The calls, held scans, cursor pulls and samples the C ABI suites share.
 */
#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`

#include <array>       // `std::array`
#include <chrono>      // `std::chrono::milliseconds`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <thread>      // `std::this_thread::sleep_for`, `std::this_thread::yield`
#include <utility>     // `std::move`

#include <yyjson.h> // `yyjson_read`, `yyjson_equals`, `yyjson_doc_get_root`, `yyjson_doc_free`

#include <forkunion/atomics.hpp> // `fu::standard_atomic_ref`

#include "ustore/ustore.hpp"

#include "engine.hpp"  // `engine_t`, `ustore_database_handle_t`, `view_t`, `database_verbs_t`, `blobs_verbs`
#include "harness.hpp" // `expect`, `expect_eq`, `expect_success`, `spin_until`

namespace unum::ustore::test::abi {

using namespace unum::ustore::utilities;

#pragma region Write And Read Calls

/** The value every deferred write stores under each of its keys, in static storage, since a
 *  deferred call borrows what its struct points to until final. */
constexpr std::string_view stored_value_k = "value";
strided_strings<std::string_view> const stored_values_k {{&stored_value_k, 0}};

/** Writes @p text under every key of @p keys in @p collection, joining @p transaction when
 *  one is given. */
inline std::size_t write_text(ustore_database_t database, ustore_collection_id_t const& collection,
                              std::span<ustore_key_t const> keys, std::string_view text,
                              ustore_transaction_t transaction = nullptr) {
    strided_strings<std::string_view> const repeated {{&text, 0}};
    ustore_outcome_t outcome {};
    ustore_blobs_write_t write {};
    write.database = database;
    write.outcome = &outcome;
    write.transaction = transaction;
    write.count = keys.size();
    write.collections = {&collection, 0};
    write.keys = {keys.data(), sizeof(ustore_key_t)};
    write.values = repeated.view();
    ustore_blobs_write(&write);
    return expect_success(outcome);
}

/** Submits one deferred write of @c stored_value_k per key of @p keys in @p collection
 *  to @p executor, each into its own outcome, borrowing @p collection until every
 *  write is final. */
inline void submit_writes(ustore_database_t database, ustore_collection_id_t const& collection,
                          ustore_executor_t executor, std::span<ustore_key_t const> keys,
                          std::span<ustore_outcome_t> outcomes) noexcept {
    for (std::size_t index = 0; index != keys.size(); ++index) {
        ustore_blobs_write_t write {};
        write.database = database;
        write.executor = executor;
        write.outcome = &outcomes[index];
        write.completion = ustore_completion_deferred_k;
        write.count = 1;
        write.collections = {&collection, 0};
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
                                                   ustore_modality_t modality) noexcept {
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
inline bool tape_written(ustore_strings_view_t const& tape) noexcept {
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

/** A blocking read of @p keys from @p collection into @p columns inside @p arena, finishing
 *  into @p outcome. */
inline ustore_blobs_read_t read_of(ustore_database_t database, ustore_collection_id_t const& collection,
                                   std::span<ustore_key_t const> keys, read_columns_t& columns, ustore_arena_t arena,
                                   ustore_outcome_t& outcome) noexcept {
    ustore_blobs_read_t read {};
    read.database = database;
    read.outcome = &outcome;
    read.count = keys.size();
    read.collections = {&collection, 0};
    read.keys = {keys.data(), sizeof(ustore_key_t)};
    read.presences = &columns.presences;
    read.values = &columns.values;
    read.arena = arena;
    return read;
}

#pragma endregion Write And Read Calls

#pragma region Held Scans

/** Reports progress once and answers, standing in for a long scan whose progress callback a
 *  case holds open. */
inline status_t scan_reporting_progress(ustore_blobs_scan_t const& arguments, view_t const&) noexcept {
    if (arguments.progress && arguments.progress(0, 1, arguments.progress_state) == ustore_cancel_k)
        return {status_code_t::cancelled_k, "The progress callback stopped the scan"};
    return {};
}

/** Leaves the stand-in database to the case holding it. */
inline void leave_database(ustore_database_t) noexcept {}

/** The stand-in engine's only database verb, which leaves the database to the case. */
database_verbs_t const progress_database_verbs_k {.database_free = &leave_database};

/** The stand-in engine's only blobs verb, a scan reporting its progress once. */
blobs_verbs<home_t::host_k> const progress_blobs_verbs_k {.scan = &scan_reporting_progress};

/** An engine serving only scans, each reporting its progress once, so a call stays unfinished while
 *  a case needs. */
engine_t const progress_engine_k {.database = &progress_database_verbs_k, .host = {.blobs = &progress_blobs_verbs_k}};

/** How far a scan held inside its progress callback got. */
enum class hold_stage_t : std::uint32_t {
    submitted_k,
    entered_k,
    released_k,
    returned_k,
};

/** A deferred scan on the stand-in engine, its callback holding it until the case releases it, with
 *  what a drain of its own executor answered from inside that callback. */
struct held_scan_t {
    ustore_database_handle_t database {&progress_engine_k};
    ustore_executor_t executor = nullptr;
    hold_stage_t stage = hold_stage_t::submitted_k;
    ustore_status_t drained = ustore_status_success_k;
    ustore_outcome_t outcome {};
    ustore_key_t start_key {};
    ustore_collection_id_t collection {};
    arena_t arena;
    ustore_blobs_scan_t scan {};
};

/** Drains its own executor, then holds the scan until released, lingering so a waiter is waiting
 *  when it finishes. */
inline ustore_continuation_t hold_until_released(ustore_size_t, ustore_size_t, void* state) noexcept {
    held_scan_t& held = *static_cast<held_scan_t*>(state);
    ustore_outcome_t drained {};
    ustore_executor_drain_t drain {.executor = held.executor, .outcome = &drained};
    ustore_executor_drain(&drain);
    held.drained = drained.status;

    fu::standard_atomic_ref<hold_stage_t> const stage {held.stage};
    stage.store(hold_stage_t::entered_k, std::memory_order_release);
    while (stage.load(std::memory_order_acquire) == hold_stage_t::entered_k) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stage.store(hold_stage_t::returned_k, std::memory_order_release);
    return ustore_continue_k;
}

/** Submits @p held to @p executor and waits until its callback holds it, so later deferred calls
 *  wait behind it. */
inline std::size_t submit_held(held_scan_t& held, ustore_executor_t executor) noexcept {
    auto [arena, arena_status] = arena_t::make(executor);
    if (arena_status.failed()) return 1;
    held.arena = std::move(arena);
    held.executor = executor;
    held.scan = {
        .database = &held.database,
        .executor = executor,
        .outcome = &held.outcome,
        .completion = ustore_completion_deferred_k,
        .count = 1,
        .collections = {&held.collection, 0},
        .start_keys = {&held.start_key, 0},
        .arena = held.arena,
        .progress = &hold_until_released,
        .progress_state = &held,
    };
    ustore_blobs_scan(&held.scan);
    fu::standard_atomic_ref<hold_stage_t> const stage {held.stage};
    return expect(
        spin_until([&stage]() noexcept { return stage.load(std::memory_order_acquire) == hold_stage_t::entered_k; }));
}

/** Lets the callback of @p held return, and waits until the scan finishes. */
inline std::size_t release_held(held_scan_t& held) noexcept {
    fu::standard_atomic_ref<hold_stage_t> {held.stage}.store(hold_stage_t::released_k, std::memory_order_release);
    ustore_outcome_wait(&held.outcome);
    return expect_success(held.outcome);
}

#pragma endregion Held Scans

#pragma region Cursor Pulls

/** The columns one pull of a blobs cursor writes into its arena, each null until the
 *  pull reports it. */
struct pulled_keys_t {
    ustore_key_t const* keys = nullptr;
    ustore_strings_view_t values {};
    ustore_size_t written = 0;

    /** The value of key @p index. */
    std::string_view value(std::size_t index) const noexcept { return string_of(tape_string(values, index)); }
};

/** A blocking pull of up to @p wanted keys and their values from @p cursor into @p pulled
 *  inside @p arena. */
inline ustore_blobs_cursor_next_t pull_of(ustore_database_t database, ustore_blobs_cursor_t cursor,
                                          pulled_keys_t& pulled, ustore_arena_t arena, ustore_size_t wanted,
                                          ustore_outcome_t& outcome) noexcept {
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

/** Counts one failure for every key of the page @p pulled that is not @p wanted in order, or
 *  reads another value. */
inline std::size_t expect_page(pulled_keys_t const& pulled, std::span<ustore_key_t const> wanted,
                               std::string_view value) {
    std::size_t failures = expect_eq(pulled.written, wanted.size());
    if (pulled.written != wanted.size()) return failures;
    for (std::size_t index = 0; index != wanted.size(); ++index) {
        failures += expect_eq(pulled.keys[index], wanted[index]);
        failures += expect_eq(pulled.value(index), value);
    }
    return failures;
}

#pragma endregion Cursor Pulls

#pragma region Wrapper Round Trips

/** Checks that every key @p reference names holds a value of @p expected_length, or is missing. */
template <typename locations_type_>
std::size_t check_length(blobs_read_ref<locations_type_>& reference, ustore_length_t expected_length) {
    std::size_t failures = 0;
    bool const expects_missing = expected_length == missing_length_k;
    using extractor_t = places_arg_extractor<locations_type_>;
    ustore_size_t const count = extractor_t {}.count(reference.locations());

    auto maybe_retrieved = reference.value();
    failures += expect(maybe_retrieved);
    if (!maybe_retrieved) return failures;
    auto const& retrieved = *maybe_retrieved;
    failures += expect_eq(retrieved.size(), count);

    auto iterator = retrieved.begin();
    for (std::size_t index = 0; index != count; ++index, ++iterator)
        failures += expect_eq((*iterator).size(), expects_missing ? 0 : expected_length);

    auto maybe_lengths = reference.length();
    failures += expect(maybe_lengths);
    for (std::size_t index = 0; index != count; ++index)
        failures += expect_eq((*maybe_lengths)[index], expected_length);

    auto maybe_indicators = reference.present();
    failures += expect(maybe_indicators);
    for (std::size_t index = 0; index != count; ++index)
        failures += expect(static_cast<bool>(maybe_indicators->at(index)) == !expects_missing);
    return failures;
}

/** Checks that the keys @p reference names hold @p values, byte for byte. */
template <template <typename> typename reference_type_, typename locations_type_>
std::size_t check_equalities(reference_type_<locations_type_>& reference, std::span<value_view_t const> values) {
    std::size_t failures = 0;
    using extractor_t = places_arg_extractor<locations_type_>;
    ustore_size_t const count = extractor_t {}.count(reference.locations());

    auto maybe_retrieved = reference.value();
    failures += expect(maybe_retrieved);
    if (!maybe_retrieved) return failures;
    auto const& retrieved = *maybe_retrieved;
    failures += expect_eq(retrieved.size(), count);

    auto iterator = retrieved.begin();
    for (std::size_t index = 0; index != count; ++index, ++iterator) {
        value_view_t const retrieved_view = *iterator;
        failures += expect_eq(retrieved_view.size(), values[index].size());
        failures += expect(retrieved_view == values[index]);
    }
    return failures;
}

/** Assigns @p values through @p reference and checks they read back as @p expected. */
template <typename locations_type_, typename values_type_>
std::size_t round_trip(blobs_ref<locations_type_>& reference, values_type_ const& values,
                       std::span<value_view_t const> expected) {
    std::size_t failures = expect(reference.assign(values).succeeded());
    failures += check_equalities(reference, expected);
    return failures;
}

/** Three one-byte values under three keys, exposed in each layout a batch of strings takes. */
struct triplet_t {
    static constexpr std::size_t value_size_k = sizeof(char);

    std::array<ustore_key_t, 3> keys {key_from_integer('a'), key_from_integer('b'), key_from_integer('c')};
    std::array<char, 3> values {'A', 'B', 'C'};
    std::array<std::uint32_t, 4> narrow_offsets {0, 1, 2, 3};
    std::array<std::uint64_t, 4> wide_offsets {0, 1, 2, 3};

    /** The values as a tape of 32-bit offsets. */
    ustore_strings_view_t u32_layout() const noexcept { return u32_tape(values.data(), narrow_offsets.data()); }

    /** The values as a tape of 64-bit offsets. */
    ustore_strings_view_t u64_layout() const noexcept { return u64_tape(values.data(), wide_offsets.data()); }

    /** The values one view each, which a write reads through an accessor. */
    std::array<value_view_t, 3> views() const noexcept {
        return {value_view_t {&values[0], 1}, value_view_t {&values[1], 1}, value_view_t {&values[2], 1}};
    }
};

/** Round-trips @p triplet through @p reference as a tape of either offset width and
 *  through an accessor. */
template <typename locations_type_>
std::size_t round_trip(blobs_ref<locations_type_>& reference, triplet_t const& triplet) {
    std::array<value_view_t, 3> const views = triplet.views();
    std::size_t failures = 0;
    failures += round_trip(reference, triplet.u32_layout(), views);
    failures += round_trip(reference, triplet.u64_layout(), views);
    failures += round_trip(reference, views, views);
    return failures;
}

/** Checks that @p reference holds @p triplet. */
template <template <typename> typename reference_type_, typename locations_type_>
std::size_t check_equalities(reference_type_<locations_type_>& reference, triplet_t const& triplet) {
    return check_equalities(reference, triplet.views());
}

#pragma endregion Wrapper Round Trips

#pragma region Modality Samples

/** Views the bytes of @p value as text. */
inline std::string_view as_text(value_view_t value) noexcept {
    return std::string_view(reinterpret_cast<char const*>(value.begin()), value.size());
}

/** Whether two JSON texts parse to equal documents; a text that does not parse equals nothing. */
inline bool json_equal(std::string_view first, std::string_view second) noexcept {
    yyjson_doc* const first_document = yyjson_read(first.data(), first.size(), 0);
    yyjson_doc* const second_document = yyjson_read(second.data(), second.size(), 0);
    bool const equal = first_document && second_document &&
                       yyjson_equals(yyjson_doc_get_root(first_document), yyjson_doc_get_root(second_document));
    yyjson_doc_free(first_document);
    yyjson_doc_free(second_document);
    return equal;
}

/** Flat documents in the compact, key-sorted form the database writes back byte for byte. */
constexpr std::array<char const*, 3> flat_documents_k {
    R"({"age":24,"person":"Alice"})",
    R"({"age":25,"person":"Bob"})",
    R"({"age":26,"person":"Carl"})",
};

/** The edge from @p source to @p target named @p id, each spelled as the key of that integer. */
constexpr edge_t edge_of(std::uint64_t source, std::uint64_t target, std::uint64_t id) noexcept {
    return {key_from_integer(source), key_from_integer(target), key_from_integer(id)};
}

#pragma endregion Modality Samples

} // namespace unum::ustore::test::abi
