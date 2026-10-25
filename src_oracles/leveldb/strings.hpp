/**
 *  @file src_oracles/leveldb/strings.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief The strings calls of the C ABI over the key ranges of `src_oracles/leveldb/database.hpp`.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstring> // `std::memcpy`

#include <string>      // `std::string`
#include <string_view> // `std::string_view`

#include <leveldb/db.h>
#include <leveldb/write_batch.h>

#include "ustore/ranges.hpp" // `strided`, `strided_iterator`

#include "database.hpp"           // `database_t`, `string_key`, `resolve_collections`, `use_snapshot`
#include "utilities/batches.hpp"  // `strings_arg_t`
#include "utilities/outputs.hpp"  // `strings_read_outputs_t`, `strings_scan_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`

namespace unum::ustore::oracles::leveldb {

#pragma region Strings

status_t strings_write(ustore_strings_write_t const& arguments, strings_arg_t const& texts, view_t const&) {
    if (!arguments.count) return {};
    strided_iterator<ustore_key_t const> const values = strided(arguments.values);
    if (!texts || !values) return {status_code_t::invalid_argument_k, "Need the strings and their values"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    status_t const resolved = resolve_collections(database, collections, arguments.count,
                                                  entry_layout_t::strings_to_uuids_k);
    if (resolved.failed()) return resolved;

    ::leveldb::WriteBatch batch;
    std::string key;
    for (std::size_t index = 0; index != arguments.count; ++index) {
        ::leveldb::Slice const value {reinterpret_cast<char const*>(values[index].uuid.bytes), sizeof(ustore_key_t)};
        batch.Put(string_key(collections[index], string_of(texts[index]), key), value);
    }
    return status_of(database.native->Write(write_options_for(arguments.durability), &batch));
}

status_t strings_remove(ustore_strings_remove_t const& arguments, strings_arg_t const& texts, view_t const&) {
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    status_t const resolved = resolve_collections(database, collections, arguments.count,
                                                  entry_layout_t::strings_to_uuids_k);
    if (resolved.failed()) return resolved;

    ::leveldb::WriteBatch batch;
    std::string key;
    for (std::size_t index = 0; index != arguments.count; ++index)
        batch.Delete(string_key(collections[index], string_of(texts[index]), key));
    return status_of(database.native->Write(write_options_for(arguments.durability), &batch));
}

status_t strings_read(ustore_strings_read_t const& arguments, strings_arg_t const& texts, view_t const& view) {
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};

    database_t& database = *handle_cast<database_t>(arguments.database);
    ::leveldb::ReadOptions options;
    use_snapshot(view, options);

    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    status_t const resolved = resolve_collections(database, collections, arguments.count,
                                                  entry_layout_t::strings_to_uuids_k);
    if (resolved.failed()) return resolved;

    auto [outputs, outputs_status] = strings_read_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;
    std::string key;
    std::string value;
    for (std::size_t index = 0; index != arguments.count; ++index) {
        native_status_t const status = database.native->Get(
            options, string_key(collections[index], string_of(texts[index]), key), &value);
        if (status.IsNotFound()) {
            outputs.push_missing();
            continue;
        }
        if (!status.ok()) return status_of(status);
        if (value.size() != sizeof(ustore_key_t))
            return {status_code_t::malformed_k, "The key a string maps onto is malformed"};
        ustore_key_t found;
        std::memcpy(found.uuid.bytes, value.data(), sizeof(found.uuid.bytes));
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

    database_t& database = *handle_cast<database_t>(arguments.database);
    ::leveldb::ReadOptions options;
    options.fill_cache = false;
    use_snapshot(view, options);

    std::size_t const count = arguments.count;
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    status_t const resolved = resolve_collections(database, collections, count, entry_layout_t::strings_to_uuids_k);
    if (resolved.failed()) return resolved;

    auto [outputs, outputs_status] = strings_scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    progress_t progress {arguments.progress, arguments.progress_state, count};
    std::string first;
    std::string bound;
    iterator_pointer_t iterator {database.native->NewIterator(options)};
    for (std::size_t task = 0; task != count; ++task) {
        ustore_collection_id_t const collection = collections[task];
        std::string_view const prefix = prefixes ? string_of(prefixes[task]) : std::string_view {};
        std::string_view const start = starts ? string_of(starts[task]) : std::string_view {};
        std::size_t const scan_end = outputs.size() + arguments.wanted;
        outputs.open_task(task);

        // The stored key of every string starting with the prefix starts with the stored key of the prefix
        ::leveldb::Slice const wanted = string_key(collection, prefix, bound);
        for (iterator->Seek(string_key(collection, start > prefix ? start : prefix, first));
             iterator->Valid() && iterator->key().starts_with(wanted) && outputs.size() != scan_end; iterator->Next())
            if (status_t const status = outputs.push(to_value(tail_of(iterator->key()))); status.failed())
                return status;
        if (!iterator->status().ok()) return status_of(iterator->status());
        if (!progress.advance(1)) return progress_cancelled_k;
    }
    return outputs.finish(arguments);
}

#pragma endregion Strings

} // namespace unum::ustore::oracles::leveldb
