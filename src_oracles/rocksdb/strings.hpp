/**
 *  @file src_oracles/rocksdb/strings.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief The strings calls of the C ABI over the column families of `src_oracles/rocksdb/database.hpp`.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstring> // `std::memcpy`

#include <algorithm>    // `std::sort`
#include <memory>       // `std::unique_ptr`
#include <mutex>        // `std::unique_lock`
#include <shared_mutex> // `std::shared_lock`
#include <string>       // `std::string`
#include <string_view>  // `std::string_view`
#include <utility>      // `std::pair`

#include <rocksdb/db.h>

#include "ustore/ranges.hpp" // `strided`, `strided_iterator`

#include "database.hpp"           // `database_t`, `transaction_t`, `refuse_collections`, `buffer_write`, `step_towards`
#include "utilities/batches.hpp"  // `validate_choices`, `validate_direction`, `samples_of`, `sample_generator_t`
#include "utilities/outputs.hpp"  // `strings_read_outputs_t`, `strings_scan_outputs_t`, `measure_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`

namespace unum::ustore::oracles::rocksdb {

#pragma region Strings

/** Points @p iterator at the first string under @p prefix from @p start, inclusive, in
 *  @p direction, where an empty @p start starts at the first string under @p prefix, carving the
 *  string after them all from @p scratch. */
status_t seek_strings(::rocksdb::Iterator& iterator, std::string_view prefix, std::string_view start,
                      ustore_scan_direction_t direction, ustore_arena_t scratch) {
    if (direction != ustore_scan_descending_k) {
        iterator.Seek(start > prefix ? start : prefix);
        return {};
    }
    bool const past_prefix = start > prefix && !start.starts_with(prefix);
    if (!start.empty() && !past_prefix) {
        iterator.SeekForPrev(start);
        return {};
    }

    // The smallest string past all under the prefix drops its trailing 0xFF bytes, bumping the last
    std::size_t length = prefix.size();
    while (length != 0 && static_cast<unsigned char>(prefix[length - 1]) == 0xFF) --length;
    if (length == 0) {
        iterator.SeekToLast();
        return {};
    }
    auto [after, status] = arena_allocate<char>(scratch, length);
    if (status.failed()) return status;
    std::memcpy(after.data(), prefix.data(), length);
    after[length - 1] = static_cast<char>(static_cast<unsigned char>(after[length - 1]) + 1);
    iterator.Seek(::rocksdb::Slice {after.data(), length});
    if (iterator.Valid()) iterator.Prev();
    else if (iterator.status().ok()) iterator.SeekToLast();
    return {};
}

/** Whether @p text comes before @p end in @p direction, where an empty @p end bounds nothing. */
bool string_before_end(std::string_view text, std::string_view end, ustore_scan_direction_t direction) noexcept {
    if (end.empty()) return true;
    return direction == ustore_scan_descending_k ? end < text : text < end;
}

status_t strings_write(ustore_strings_write_t const& arguments, strings_arg_t const& texts, view_t const& view) {
    if (status_t const status = validate_choices(view.transaction, arguments.durability, view.watch); status.failed())
        return status;
    if (!arguments.count) return {};
    strided_iterator<ustore_key_t const> const values = strided(arguments.values);
    if (!texts || !values) return {status_code_t::invalid_argument_k, "Need the strings and their values"};

    database_t& database = *state_of<database_t>(arguments.database);
    transaction_t* const transaction = state_of<transaction_t>(view.transaction);
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);

    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, collections, arguments.count,
                                                entry_layout_t::strings_to_uuids_k);
    if (refused.failed()) return refused;
    if (status_t const status = refuse_staged_write(transaction); status.failed()) return status;

    ::rocksdb::WriteBatch batch;
    for (std::size_t index = 0; index != arguments.count; ++index) {
        ustore_collection_id_t const collection = collections[index];
        column_t* const column = column_of(database, collection);
        ::rocksdb::Slice const key {string_of(texts[index])};
        ::rocksdb::Slice const value = to_slice(values[index]);
        native_status_t const status = transaction ? buffer_write(*transaction, column, collection, key, value)
                                                   : batch.Put(column, key, value);
        if (!status.ok()) return status_of(status);
    }
    if (transaction) return {};
    return status_of(database.native->Write(write_options_for(arguments.durability), &batch));
}

/** Removes every string from each of @p texts up to its end in @p ends, exclusive, or to the last
 *  string where the end is empty, string by string: through @p transaction, or in one batch at the
 *  head. The caller holds @c columns_mutex, exclusively at the head, so no write lands in a range
 *  between its listing and its removal. */
status_t remove_string_ranges(database_t& database, transaction_t* transaction,
                              strided_iterator<ustore_collection_id_t const> collections, std::size_t count,
                              strings_arg_t const& texts, strings_arg_t const& ends, ustore_durability_t durability,
                              view_t const& view) {
    ::rocksdb::WriteBatch batch;
    ::rocksdb::ReadOptions read_options;
    if (transaction) read_options.snapshot = snapshot_of(*transaction);
    growing_strings<arena_allocator<byte_t>> listed {view.scratch};
    for (std::size_t index = 0; index != count; ++index) {
        ustore_collection_id_t const collection = collections[index];
        column_t* const column = column_of(database, collection);
        std::string_view const end = string_of(ends[index]);
        std::unique_ptr<::rocksdb::Iterator> iterator {new_iterator(database, transaction, read_options, column)};
        if (!iterator) return {status_code_t::out_of_memory_k, "Failed to create a RocksDB iterator"};

        // Changing a transaction's batch under its iterator breaks it, so the range is listed first
        std::size_t const first = listed.size();
        for (iterator->Seek(string_of(texts[index]));
             iterator->Valid() && string_before_end(iterator->key().ToStringView(), end, ustore_scan_ascending_k);
             iterator->Next())
            if (status_t const status = listed.push_back(bytes_of(iterator->key().ToStringView())); status.failed())
                return status;
        if (!iterator->status().ok()) return status_of(iterator->status());
        for (std::size_t listed_index = first; listed_index != listed.size(); ++listed_index) {
            ::rocksdb::Slice const key {string_of(listed[listed_index])};
            native_status_t const status = transaction ? buffer_remove(*transaction, column, collection, key)
                                                       : batch.Delete(column, key);
            if (!status.ok()) return status_of(status);
        }
    }
    if (transaction || !batch.Count()) return {};
    return status_of(database.native->Write(write_options_for(durability), &batch));
}

status_t strings_remove(ustore_strings_remove_t const& arguments, strings_arg_t const& texts, strings_arg_t const& ends,
                        view_t const& view) {
    if (status_t const status = validate_choices(view.transaction, arguments.durability, view.watch); status.failed())
        return status;
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};

    database_t& database = *state_of<database_t>(arguments.database);
    transaction_t* const transaction = state_of<transaction_t>(view.transaction);
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);

    if (ends && !transaction) {
        std::unique_lock lock {database.columns_mutex};
        status_t const refused = refuse_collections(database, collections, arguments.count,
                                                    entry_layout_t::strings_to_uuids_k);
        if (refused.failed()) return refused;
        return remove_string_ranges(database, transaction, collections, arguments.count, texts, ends,
                                    arguments.durability, view);
    }
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, collections, arguments.count,
                                                entry_layout_t::strings_to_uuids_k);
    if (refused.failed()) return refused;
    if (status_t const status = refuse_staged_write(transaction); status.failed()) return status;
    if (ends)
        return remove_string_ranges(database, transaction, collections, arguments.count, texts, ends,
                                    arguments.durability, view);

    ::rocksdb::WriteBatch batch;
    for (std::size_t index = 0; index != arguments.count; ++index) {
        ustore_collection_id_t const collection = collections[index];
        column_t* const column = column_of(database, collection);
        ::rocksdb::Slice const key {string_of(texts[index])};
        native_status_t const status = transaction ? buffer_remove(*transaction, column, collection, key)
                                                   : batch.Delete(column, key);
        if (!status.ok()) return status_of(status);
    }
    if (transaction) return {};
    return status_of(database.native->Write(write_options_for(arguments.durability), &batch));
}

status_t strings_read(ustore_strings_read_t const& arguments, strings_arg_t const& texts, view_t const& view) {
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};

    database_t& database = *state_of<database_t>(arguments.database);
    ::rocksdb::ReadOptions read_options;
    transaction_t* const transaction = reading_transaction(view, read_options);
    if (status_t const status = refuse_staged_watch(transaction, view.watch); status.failed()) return status;

    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, collections, arguments.count,
                                                entry_layout_t::strings_to_uuids_k);
    if (refused.failed()) return refused;

    auto [outputs, outputs_status] = strings_read_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;
    ::rocksdb::PinnableSlice value;
    for (std::size_t index = 0; index != arguments.count; ++index) {
        ustore_collection_id_t const collection = collections[index];
        column_t* const column = column_of(database, collection);
        std::string_view const text = string_of(texts[index]);
        ::rocksdb::Slice const key {text};
        if (transaction && view.watch == ustore_watch_keys_k &&
            st::failed(transaction->watched_strings.push_back(std::pair {collection, std::string(text)})))
            return {status_code_t::out_of_memory_k, "Failed to watch a string"};
        value.Reset();
        native_status_t const status = transaction ? transaction->batch.GetFromBatchAndDB(
                                                         database.native.get(), read_options, column, key, &value)
                                                   : database.native->Get(read_options, column, key, &value);
        if (status.IsNotFound()) {
            outputs.assign_missing(index);
            continue;
        }
        if (!status.ok()) return status_of(status);
        if (value.size() != sizeof(ustore_key_t))
            return {status_code_t::malformed_k, "The database holds a malformed strings entry"};
        ustore_key_t found;
        std::memcpy(&found, value.data(), sizeof(ustore_key_t));
        outputs.assign_present(index, found);
    }
    outputs.finish(arguments);
    return {};
}

status_t strings_scan(ustore_strings_scan_t const& arguments, strings_arg_t const& prefixes,
                      strings_arg_t const& starts, strings_arg_t const& ends, view_t const& view) {
    if (status_t const status = validate_direction(arguments.direction); status.failed()) return status;
    if (!arguments.count) return {};
    if (!arguments.offsets || !arguments.strings)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the strings"};

    database_t& database = *state_of<database_t>(arguments.database);
    ::rocksdb::ReadOptions read_options;
    read_options.fill_cache = false;
    transaction_t* const transaction = reading_transaction(view, read_options);

    std::size_t const count = arguments.count;
    ustore_scan_direction_t const direction = arguments.direction;
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, collections, count, entry_layout_t::strings_to_uuids_k);
    if (refused.failed()) return refused;

    auto [outputs, outputs_status] = strings_scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    progress_t progress {arguments.progress, arguments.progress_state, count};
    for (std::size_t task = 0; task != count; ++task) {
        std::string_view const prefix = prefixes ? string_of(prefixes[task]) : std::string_view {};
        std::string_view const start = starts ? string_of(starts[task]) : std::string_view {};
        std::string_view const end = ends ? string_of(ends[task]) : std::string_view {};
        std::size_t const scan_end = outputs.size() + arguments.wanted;
        outputs.open_task(task);

        std::unique_ptr<::rocksdb::Iterator> iterator {
            new_iterator(database, transaction, read_options, column_of(database, collections[task]))};
        if (!iterator) return {status_code_t::out_of_memory_k, "Failed to create a RocksDB iterator"};
        if (status_t const status = seek_strings(*iterator, prefix, start, direction, view.scratch); status.failed())
            return status;
        for (; iterator->Valid() && outputs.size() != scan_end; step_towards(*iterator, direction)) {
            std::string_view const text = iterator->key().ToStringView();
            if (!text.starts_with(prefix) || !string_before_end(text, end, direction)) break;
            if (status_t const status = outputs.push(text); status.failed()) return status;
        }
        if (!iterator->status().ok()) return status_of(iterator->status());
        if (!progress.advance(1)) return progress_cancelled_k;
    }
    return outputs.finish(arguments);
}

/** Samples strings by drawing a reservoir of positions over one walk of every collection, then
 *  listing the strings at those positions on a second walk of the same iterator, which sees
 *  the same state. */
status_t strings_sample(ustore_strings_sample_t const& arguments, view_t const& view) {
    if (!arguments.count) return {};
    if (!arguments.offsets || !arguments.strings)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the strings"};

    database_t& database = *state_of<database_t>(arguments.database);
    ::rocksdb::ReadOptions read_options;
    read_options.fill_cache = false;
    transaction_t* const transaction = reading_transaction(view, read_options);

    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    sample_batch_t const samples = samples_of(arguments);
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, collections, arguments.count,
                                                entry_layout_t::strings_to_uuids_k);
    if (refused.failed()) return refused;

    auto [outputs, outputs_status] = strings_scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    sample_generator_t generator = sample_generator_t::seeded(&outputs);
    st::vector<std::size_t, arena_allocator<std::size_t>> kept {{view.scratch}};
    for (std::size_t task_index = 0; task_index != samples.size(); ++task_index) {
        sample_task_t const sample = samples[task_index];
        outputs.open_task(task_index);
        if (!sample.limit) continue;
        std::unique_ptr<::rocksdb::Iterator> iterator {
            new_iterator(database, transaction, read_options, column_of(database, sample.collection))};
        if (!iterator) return {status_code_t::out_of_memory_k, "Failed to create a RocksDB iterator"};

        kept.clear();
        std::size_t seen = 0;
        for (iterator->SeekToFirst(); iterator->Valid(); iterator->Next(), ++seen) {
            std::size_t const slot = seen < sample.limit ? seen : st::draw_below(generator, seen + 1);
            if (slot >= sample.limit) continue;
            if (slot != kept.size()) kept[slot] = seen;
            else if (status_t const status = growth_status(kept.emplace_back(seen)); status.failed()) return status;
        }
        if (!iterator->status().ok()) return status_of(iterator->status());

        std::sort(kept.begin(), kept.end());
        std::size_t position = 0;
        std::size_t next = 0;
        for (iterator->SeekToFirst(); iterator->Valid() && next != kept.size(); iterator->Next(), ++position) {
            if (position != kept[next]) continue;
            if (status_t const status = outputs.push(iterator->key().ToStringView()); status.failed()) return status;
            ++next;
        }
        if (!iterator->status().ok()) return status_of(iterator->status());
    }
    return outputs.finish(arguments);
}

/** Measures every string range by walking it at the view it reads: its strings, the keys they map
 *  onto as their value bytes, and both with a batch record's overhead as their space. */
status_t strings_measure(ustore_strings_measure_t const& arguments, strings_arg_t const& starts,
                         strings_arg_t const& ends, view_t const& view) {
    if (status_t const status = validate_direction(arguments.direction); status.failed() || !arguments.count)
        return status;

    database_t& database = *state_of<database_t>(arguments.database);
    ::rocksdb::ReadOptions read_options;
    read_options.fill_cache = false;
    transaction_t* const transaction = reading_transaction(view, read_options);

    ustore_scan_direction_t const direction = arguments.direction;
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    std::shared_lock lock {database.columns_mutex};
    status_t const refused = refuse_collections(database, collections, arguments.count,
                                                entry_layout_t::strings_to_uuids_k);
    if (refused.failed()) return refused;

    auto [outputs, outputs_status] = measure_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;
    for (std::size_t task = 0; task != arguments.count; ++task) {
        std::string_view const start = starts ? string_of(starts[task]) : std::string_view {};
        std::string_view const end = ends ? string_of(ends[task]) : std::string_view {};
        std::unique_ptr<::rocksdb::Iterator> iterator {
            new_iterator(database, transaction, read_options, column_of(database, collections[task]))};
        if (!iterator) return {status_code_t::out_of_memory_k, "Failed to create a RocksDB iterator"};
        if (status_t const status = seek_strings(*iterator, {}, start, direction, view.scratch); status.failed())
            return status;

        ustore_size_t cardinality = 0;
        ustore_size_t string_bytes = 0;
        for (; iterator->Valid(); step_towards(*iterator, direction)) {
            std::string_view const text = iterator->key().ToStringView();
            if (!string_before_end(text, end, direction)) break;
            ++cardinality;
            string_bytes += text.size();
        }
        if (!iterator->status().ok()) return status_of(iterator->status());
        ustore_size_t const value_bytes = cardinality * sizeof(ustore_key_t);
        outputs.assign(task, cardinality, value_bytes, string_bytes + value_bytes + cardinality * record_overhead_k);
    }
    outputs.finish(arguments);
    return {};
}

#pragma endregion Strings

} // namespace unum::ustore::oracles::rocksdb
