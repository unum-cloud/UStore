/**
 *  @file src_oracles/rocksdb/strings.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief The strings calls of the C ABI over the column families of `src_oracles/rocksdb/database.hpp`.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstring> // `std::memcpy`

#include <memory>       // `std::unique_ptr`
#include <shared_mutex> // `std::shared_lock`
#include <string>       // `std::string`
#include <string_view>  // `std::string_view`
#include <utility>      // `std::pair`

#include <rocksdb/db.h>

#include "ustore/ranges.hpp" // `strided`, `strided_iterator`

#include "database.hpp"           // `database_t`, `transaction_t`, `refuse_collections`, `buffer_write`, `watch`
#include "utilities/batches.hpp"  // `strings_arg`
#include "utilities/outputs.hpp"  // `strings_read_outputs_t`, `strings_scan_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`

namespace unum::ustore::oracles::rocksdb {

#pragma region Strings

status_t strings_write(ustore_strings_write_t const& arguments, strings_arg_t const& texts, view_t const& view) {
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

status_t strings_remove(ustore_strings_remove_t const& arguments, strings_arg_t const& texts, view_t const& view) {
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};

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
            outputs.push_missing();
            continue;
        }
        if (!status.ok()) return status_of(status);
        if (value.size() != sizeof(ustore_key_t))
            return {status_code_t::malformed_k, "The database holds a malformed strings entry"};
        ustore_key_t found;
        std::memcpy(&found, value.data(), sizeof(ustore_key_t));
        outputs.push_present(found);
    }
    outputs.finish(arguments);
    return {};
}

status_t strings_scan(ustore_strings_scan_t const& arguments, strings_arg_t const& prefixes,
                      strings_arg_t const& starts, view_t const& view) {
    if (!arguments.count) return {};
    if (!arguments.offsets || !arguments.strings)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the strings"};

    database_t& database = *state_of<database_t>(arguments.database);
    ::rocksdb::ReadOptions read_options;
    read_options.fill_cache = false;
    transaction_t* const transaction = reading_transaction(view, read_options);

    std::size_t const count = arguments.count;
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
        std::string_view const first = start > prefix ? start : prefix;
        std::size_t const scan_end = outputs.size() + arguments.wanted;
        outputs.open_task(task);

        std::unique_ptr<::rocksdb::Iterator> iterator {
            new_iterator(database, transaction, read_options, column_of(database, collections[task]))};
        for (iterator->Seek(::rocksdb::Slice(first.data(), first.size()));
             iterator->Valid() && outputs.size() != scan_end; iterator->Next()) {
            std::string_view const text = iterator->key().ToStringView();
            if (!text.starts_with(prefix)) break;
            if (status_t const status = outputs.push(text); status.failed()) return status;
        }
        if (!iterator->status().ok()) return status_of(iterator->status());
        if (!progress.advance(1)) return progress_cancelled_k;
    }
    return outputs.finish(arguments);
}

#pragma endregion Strings

} // namespace unum::ustore::oracles::rocksdb
