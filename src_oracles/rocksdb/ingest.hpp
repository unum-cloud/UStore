/**
 *  @file src_oracles/rocksdb/ingest.hpp
 *  @author Ash Vardanian
 *  @date November 18, 2026
 *  @brief The ingest of <tt>c/ustore/db.h</tt> for the RocksDB engine: rows go into SST files
 *      beside the database, and one @c IngestExternalFile moves them into its column family.
 *
 *  Blobs, documents and pages ingest; strings and graphs answer unsupported, as their column
 *  families key rows the writes lay out themselves. A sorted ingest writes every append into the
 *  open file as it arrives, its keys checked against the last one before them, and rolls to a new
 *  file once one fills. An unsorted ingest copies every row into the ingest's arena and writes them
 *  sorted at the finish, the last row of a key winning. RocksDB validates a transaction's locked
 *  keys only against sequence numbers its memtables and files hold, which an ingested file at the
 *  bottom level may not, so each ingest records its key range, and the transactions that began
 *  before it and touched a key there fail at stage.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uintptr_t`
#include <cstring> // `std::memcmp`

#include <algorithm>    // `std::stable_sort`
#include <filesystem>   // `std::filesystem::remove`
#include <memory>       // `std::unique_ptr`, `std::destroy_at`
#include <mutex>        // `std::unique_lock`
#include <new>          // placement `new`
#include <shared_mutex> // `std::shared_lock`
#include <span>         // `std::span`
#include <string>       // `std::string`, the paths RocksDB takes
#include <string_view>  // `std::string_view`
#include <system_error> // `std::error_code`
#include <vector>       // `std::vector`, the file list RocksDB takes

#include <fmt/format.h> // `fmt::format`

#include <rocksdb/sst_file_writer.h>

#include "ustore/db.h" // `ustore_ingest_init_t`, `ustore_ingest_append_t`, `ustore_ingest_finish_t`

#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`, `arena_copy`, `arena_allocator`

#include "ingest_layer.hpp"      // `parse_ingest_values`, `ingest_values_of`
#include "pages_layer.hpp"       // `clock_stamp`
#include "utilities/batches.hpp" // `keyed_rows_of`

#include "database.hpp" // `database_t`, `ingested_range_t`, `status_of`, `to_slice`

namespace unum::ustore::oracles::rocksdb {

/** Bytes of rows an ingest writes into one SST file before it starts the next. */
constexpr std::size_t ingest_file_bytes_k = std::size_t {256} << 20;

/** An ingest under way: its database and collection, the SST files written so far and the one open,
 *  the range of keys they hold, and the rows an unsorted ingest keeps in its arena till the end. */
struct ingest_state_t {
    database_t& database;
    ustore_collection_id_t collection;
    ustore_ingest_order_t order;

    /** What the rows land as, and where the next page parsed is placed. */
    ingest_values_t lands_as;
    pages::placement_t placement {clock_stamp(), 0};

    /** The arena the ingest lives in, which held rows are copied into. */
    ustore_arena_t kept;

    /** The SST files written, the last one still open in @c writer while it takes rows. */
    std::vector<std::string> files;
    std::unique_ptr<::rocksdb::SstFileWriter> writer;
    std::size_t file_bytes = 0;

    /** The smallest and the largest key written, empty before the first. */
    std::string lower;
    std::string upper;

    /** The keys and values an unsorted ingest holds in order of arrival. */
    st::vector<ustore_key_t, arena_allocator<ustore_key_t>> held_keys;
    st::vector<std::span<byte_t const>, arena_allocator<std::span<byte_t const>>> held_values;

    ingest_state_t(database_t& database, ustore_collection_id_t collection, ustore_ingest_order_t order,
                   ingest_values_t lands_as, ustore_arena_t kept)
        : database(database), collection(collection), order(order), lands_as(lands_as), kept(kept), held_keys({kept}),
          held_values({kept}) {}
    ingest_state_t(ingest_state_t const&) = delete;
    ingest_state_t& operator=(ingest_state_t const&) = delete;

    /** Abandons the open file and removes every file no finish moved into the database. */
    ~ingest_state_t() noexcept {
        writer.reset();
        for (std::string const& file : files) {
            std::error_code error;
            std::filesystem::remove(file, error);
        }
    }
};

/** The directory an ingest writes its files into: the first data directory, or the database's. */
inline std::string ingest_directory_of(database_t& database) {
    ::rocksdb::DBOptions const options = database.native->GetDBOptions();
    return options.db_paths.empty() ? database.native->GetName() : options.db_paths[0].path;
}

/** Writes @p key and @p value into the open file of @p ingest, opening one and rolling to the next
 *  once it fills; the caller holds @c columns_mutex shared, so the column family stays alive. */
inline native_status_t write_row(ingest_state_t& ingest, column_t* column, ::rocksdb::Slice key,
                                 ::rocksdb::Slice value) {
    if (!ingest.writer || ingest.file_bytes >= ingest_file_bytes_k) {
        if (ingest.writer) {
            native_status_t const closed = ingest.writer->Finish();
            if (!closed.ok()) return closed;
        }
        ingest.files.push_back(fmt::format("{}/ustore-ingest-{:x}-{}.sst", ingest_directory_of(ingest.database),
                                           reinterpret_cast<std::uintptr_t>(&ingest), ingest.files.size()));
        ingest.writer = std::make_unique<::rocksdb::SstFileWriter>(::rocksdb::EnvOptions(),
                                                                   ingest.database.native->GetOptions(column), column);
        native_status_t const opened = ingest.writer->Open(ingest.files.back());
        if (!opened.ok()) return opened;
        ingest.file_bytes = 0;
    }
    native_status_t const put = ingest.writer->Put(key, value);
    if (!put.ok()) return put;
    if (ingest.lower.empty()) ingest.lower.assign(key.data(), key.size());
    ingest.upper.assign(key.data(), key.size());
    ingest.file_bytes += key.size() + value.size();
    return put;
}

/** The column family of the collection @p ingest fills, or null once it was dropped; the caller
 *  holds @c columns_mutex. */
inline column_t* ingested_column(ingest_state_t& ingest) noexcept {
    return column_of(ingest.database, ingest.collection);
}

/** Implements @c ustore_ingest_init, refusing strings and graphs and every collection no caller
 *  created. */
inline status_t ingest_init_(ustore_ingest_init_t const& arguments, view_t const&) {
    database_t& database = *state_of<database_t>(arguments.database);
    ustore_modality_t modality {};
    {
        std::shared_lock lock {database.columns_mutex};
        auto const found = database.columns.find(arguments.collection);
        if (found == database.columns.end()) return {status_code_t::not_found_k, "No such collection"};
        modality = (*found).mapped.modality;
    }
    if (modality == ustore_modality_strings_k || modality == ustore_modality_graph_k)
        return {status_code_t::unsupported_k, "RocksDB ingests blobs, documents and pages alone"};
    ustore_arena_t const kept = &(*arguments.ingest)->kept;
    auto [room, room_status] = arena_allocate<ingest_state_t>(kept, 1);
    if (room_status.failed()) return room_status;
    (*arguments.ingest)->state = new (room.data())
        ingest_state_t {database, arguments.collection, arguments.order, ingest_values_of(modality), kept};
    return {};
}

/** Writes a sorted append into the open files, all of its keys checked first, so a refused append
 *  writes nothing. */
inline status_t write_sorted_(ingest_state_t& ingest, utilities::keyed_rows_t const& rows,
                              std::span<std::span<byte_t const> const> values) {
    std::string_view previous = ingest.upper;
    for (std::size_t row = 0; row != rows.size(); ++row) {
        ::rocksdb::Slice const key = to_slice(rows.keys[row]);
        if (!previous.empty() && key.ToStringView() <= previous)
            return {status_code_t::invalid_argument_k, "A sorted ingest got an unsorted key"};
        previous = key.ToStringView();
    }
    std::shared_lock lock {ingest.database.columns_mutex};
    column_t* const column = ingested_column(ingest);
    if (!column) return {status_code_t::not_found_k, "The collection was dropped while the ingest ran"};
    for (std::size_t row = 0; row != rows.size(); ++row) {
        std::span<byte_t const> const value = values[row];
        native_status_t const written = write_row(
            ingest, column, to_slice(rows.keys[row]),
            ::rocksdb::Slice {reinterpret_cast<char const*>(value.data()), value.size()});
        if (!written.ok()) return status_of(written);
    }
    return {};
}

/** Copies an unsorted append into the ingest's arena until the finish, which a failure leaves as it
 *  was. */
inline status_t hold_unsorted_(ingest_state_t& ingest, utilities::keyed_rows_t const& rows,
                               std::span<std::span<byte_t const> const> values) noexcept {
    std::size_t const before = ingest.held_keys.size();
    status_t status = growth_status(ingest.held_keys.reserve(before + rows.size()));
    if (status.succeeded()) status = growth_status(ingest.held_values.reserve(before + rows.size()));
    for (std::size_t row = 0; status.succeeded() && row != rows.size(); ++row) {
        auto [copy, copy_status] = arena_copy<byte_t>(ingest.kept, values[row]);
        if ((status = copy_status).failed()) break;
        [[maybe_unused]] st::status_t placed = ingest.held_keys.push_back(st::assume_reserved,
                                                                          ustore_key_t {rows.keys[row]});
        placed = ingest.held_values.push_back(st::assume_reserved, std::span<byte_t const> {copy});
    }
    while (status.failed() && ingest.held_keys.size() != before)
        ingest.held_keys.pop_back(), ingest.held_values.pop_back();
    return status;
}

/** Implements @c ustore_ingest_append, parsing the values over the slices of @p view. */
inline status_t ingest_append_(ustore_ingest_append_t const& arguments, strings_arg_t const& values,
                               view_t const& view) {
    using strings_kit_t = strings_kit_of<cpu_ladder_floor_k>;
    ingest_state_t& ingest = *state_of<ingest_state_t>(arguments.ingest);
    auto [rows, rows_status] = utilities::keyed_rows_of(arguments, values);
    if (rows_status.failed() || !rows.size()) return rows_status;
    auto [parsed, parsed_status] = parse_ingest_values<strings_kit_t>(ingest.lands_as, rows, ingest.placement, view);
    if (parsed_status.failed()) return parsed_status;
    status_t const appended = ingest.order == ustore_ingest_sorted_k ? write_sorted_(ingest, rows, parsed)
                                                                     : hold_unsorted_(ingest, rows, parsed);
    if (appended.succeeded()) ingest.placement.sequence += static_cast<std::uint32_t>(rows.size());
    return appended;
}

/** Writes what an unsorted ingest held in key order, the last row of a key winning, its order
 *  carved in @p scratch; the caller holds @c columns_mutex shared. */
inline status_t write_held_(ingest_state_t& ingest, column_t* column, ustore_arena_t scratch) {
    std::size_t const count = ingest.held_keys.size();
    auto [order, order_status] = arena_allocate<std::size_t>(scratch, count);
    if (order_status.failed()) return order_status;
    for (std::size_t index = 0; index != count; ++index) order[index] = index;
    auto const precedes = [&](std::size_t left, std::size_t right) noexcept {
        return std::memcmp(&ingest.held_keys[left], &ingest.held_keys[right], sizeof(ustore_key_t)) < 0;
    };
    std::stable_sort(order.begin(), order.end(), precedes);
    for (std::size_t position = 0; position != count; ++position) {
        // A stable sort keeps a repeated key's rows in order, so a run ends on its last row written
        if (position + 1 != count && !precedes(order[position], order[position + 1])) continue;
        std::span<byte_t const> const value = ingest.held_values[order[position]];
        native_status_t const written = write_row(
            ingest, column, to_slice(ingest.held_keys[order[position]]),
            ::rocksdb::Slice {reinterpret_cast<char const*>(value.data()), value.size()});
        if (!written.ok()) return status_of(written);
    }
    return {};
}

/** Implements @c ustore_ingest_finish, moving every file into the column family in one ingestion
 *  and recording the range it landed, so a transaction that began before it and touched a key there
 *  fails at stage. */
inline status_t ingest_finish_(ustore_ingest_finish_t const& arguments, view_t const& view) {
    ingest_state_t& ingest = *state_of<ingest_state_t>(arguments.ingest);
    database_t& database = ingest.database;
    std::shared_lock lock {database.columns_mutex};
    column_t* const column = ingested_column(ingest);
    if (!column) return {status_code_t::not_found_k, "The collection was dropped while the ingest ran"};
    if (ingest.order == ustore_ingest_unsorted_k)
        if (status_t const written = write_held_(ingest, column, view.scratch); written.failed()) return written;
    if (ingest.writer) {
        native_status_t const closed = ingest.writer->Finish();
        ingest.writer.reset();
        if (!closed.ok()) return status_of(closed);
    }
    // The range's record is reserved before the files land, so a landed ingest is always recorded
    std::unique_lock guard {database.ingested_mutex};
    if (!ingest.lower.empty() && st::failed(database.ingested.reserve(database.ingested.size() + 1)))
        return {status_code_t::out_of_memory_k, "No room to record the ingest"};
    if (!ingest.files.empty()) {
        ::rocksdb::IngestExternalFileOptions options;
        options.move_files = true;
        native_status_t const landed = database.native->IngestExternalFile(column, ingest.files, options);
        if (!landed.ok()) return status_of(landed);
        ingest.files.clear();
    }
    ::rocksdb::SequenceNumber const stamp = database.native->GetLatestSequenceNumber();
    if (!ingest.lower.empty()) [[maybe_unused]]
        st::status_t const recorded = database.ingested.push_back(
            st::assume_reserved, ingested_range_t {ingest.collection, ingest.lower, ingest.upper, stamp});
    guard.unlock();
    if (arguments.commit_stamp) *arguments.commit_stamp = static_cast<ustore_stamp_t>(stamp);
    return {};
}

status_t ingest_init(ustore_ingest_init_t const& arguments, view_t const& view) noexcept {
    return guarded<&ingest_init_>(arguments, view);
}

status_t ingest_append(ustore_ingest_append_t const& arguments, strings_arg_t const& values,
                       view_t const& view) noexcept {
    return guarded<&ingest_append_>(arguments, values, view);
}

status_t ingest_finish(ustore_ingest_finish_t const& arguments, view_t const& view) noexcept {
    return guarded<&ingest_finish_>(arguments, view);
}

/** Implements @c ustore_ingest_free, removing every file never moved into the database. */
void ingest_free(ustore_ingest_t handle) noexcept { std::destroy_at(state_of<ingest_state_t>(handle)); }

} // namespace unum::ustore::oracles::rocksdb
