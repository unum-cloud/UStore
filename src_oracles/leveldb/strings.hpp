/**
 *  @file src_oracles/leveldb/strings.hpp
 *  @author Ash Vardanian
 *  @date January 9, 2023
 *  @brief The strings calls of the C ABI over the key ranges of `src_oracles/leveldb/database.hpp`.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstring> // `std::memcpy`

#include <algorithm>   // `std::sort`
#include <string>      // `std::string`
#include <string_view> // `std::string_view`

#include <leveldb/db.h>
#include <leveldb/write_batch.h>

#include "ustore/ranges.hpp" // `strided`, `strided_iterator`

#include "database.hpp"           // `database_t`, `string_key`, `seek_towards`, `seek_before`, `step_towards`
#include "utilities/batches.hpp"  // `validate_choices`, `validate_direction`, `samples_of`, `sample_generator_t`
#include "utilities/outputs.hpp"  // `strings_read_outputs_t`, `strings_scan_outputs_t`, `measure_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`

namespace unum::ustore::oracles::leveldb {

#pragma region Strings

/** Points @p iterator at the first string of @p collection under @p prefix from @p start,
 *  inclusive, in @p direction, where an empty @p start starts at the first string under @p prefix,
 *  spelling the keys it seeks in @p stored. */
void seek_strings(::leveldb::Iterator& iterator, ustore_collection_id_t const& collection, std::string_view prefix,
                  std::string_view start, ustore_scan_direction_t direction, std::string& stored) {
    if (direction != ustore_scan_descending_k) {
        iterator.Seek(string_key(collection, start > prefix ? start : prefix, stored));
        return;
    }
    bool const past_prefix = start > prefix && !start.starts_with(prefix);
    if (!start.empty() && !past_prefix) {
        seek_towards(iterator, string_key(collection, start, stored), direction);
        return;
    }

    // The smallest key past all under the prefix drops its trailing 0xFF bytes, bumping the last
    string_key(collection, prefix, stored);
    while (!stored.empty() && static_cast<unsigned char>(stored.back()) == 0xFF) stored.pop_back();
    if (stored.empty()) {
        iterator.SeekToLast();
        return;
    }
    stored.back() = static_cast<char>(static_cast<unsigned char>(stored.back()) + 1);
    seek_before(iterator, stored);
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

status_t strings_remove(ustore_strings_remove_t const& arguments, strings_arg_t const& texts, strings_arg_t const& ends,
                        view_t const& view) {
    if (status_t const status = validate_choices(view.transaction, arguments.durability, view.watch); status.failed())
        return status;
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};

    database_t& database = *state_of<database_t>(arguments.database);
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    status_t const resolved = resolve_collections(database, collections, arguments.count,
                                                  entry_layout_t::strings_to_uuids_k);
    if (resolved.failed()) return resolved;

    ::leveldb::WriteBatch batch;
    std::string key;
    if (!ends) {
        for (std::size_t index = 0; index != arguments.count; ++index)
            batch.Delete(string_key(collections[index], string_of(texts[index]), key));
        return status_of(database.native->Write(write_options_for(arguments.durability), &batch));
    }

    // LevelDB has no range tombstones, so the ranges' strings list into a batch from one version
    iterator_pointer_t iterator {database.native->NewIterator(::leveldb::ReadOptions())};
    for (std::size_t index = 0; index != arguments.count; ++index) {
        ustore_collection_id_t const collection = collections[index];
        std::string_view const end = string_of(ends[index]);
        ::leveldb::Slice const strings = prefix_of(collection);
        for (iterator->Seek(string_key(collection, string_of(texts[index]), key));
             iterator->Valid() && iterator->key().starts_with(strings) &&
             string_before_end(to_value(tail_of(iterator->key())), end, ustore_scan_ascending_k);
             iterator->Next())
            batch.Delete(iterator->key());
        if (!iterator->status().ok()) return status_of(iterator->status());
    }
    return status_of(database.native->Write(write_options_for(arguments.durability), &batch));
}

status_t strings_read(ustore_strings_read_t const& arguments, strings_arg_t const& texts, view_t const& view) {
    if (!arguments.count) return {};
    if (!texts) return {status_code_t::invalid_argument_k, "No strings were provided"};

    database_t& database = *state_of<database_t>(arguments.database);
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
            outputs.assign_missing(index);
            continue;
        }
        if (!status.ok()) return status_of(status);
        if (value.size() != sizeof(ustore_key_t))
            return {status_code_t::malformed_k, "The key a string maps onto is malformed"};
        ustore_key_t found;
        std::memcpy(found.uuid.bytes, value.data(), sizeof(found.uuid.bytes));
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
    ::leveldb::ReadOptions options;
    options.fill_cache = false;
    use_snapshot(view, options);

    std::size_t const count = arguments.count;
    ustore_scan_direction_t const direction = arguments.direction;
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
        std::string_view const end = ends ? string_of(ends[task]) : std::string_view {};
        std::size_t const scan_end = outputs.size() + arguments.wanted;
        outputs.open_task(task);

        // The stored key of every string starting with the prefix starts with the stored key of the prefix
        ::leveldb::Slice const wanted = string_key(collection, prefix, bound);
        for (seek_strings(*iterator, collection, prefix, start, direction, first);
             iterator->Valid() && iterator->key().starts_with(wanted) && outputs.size() != scan_end;
             step_towards(*iterator, direction)) {
            value_view_t const text = to_value(tail_of(iterator->key()));
            if (!string_before_end(text, end, direction)) break;
            if (status_t const status = outputs.push(text); status.failed()) return status;
        }
        if (!iterator->status().ok()) return status_of(iterator->status());
        if (!progress.advance(1)) return progress_cancelled_k;
    }
    return outputs.finish(arguments);
}

/** Samples strings by drawing a reservoir of positions over one walk of every collection, then
 *  listing the strings at those positions on a second walk of the same iterator, which sees
 *  the same version. */
status_t strings_sample(ustore_strings_sample_t const& arguments, view_t const& view) {
    if (!arguments.count) return {};
    if (!arguments.offsets || !arguments.strings)
        return {status_code_t::invalid_argument_k, "Need outputs for the offsets and the strings"};

    database_t& database = *state_of<database_t>(arguments.database);
    ::leveldb::ReadOptions options;
    options.fill_cache = false;
    use_snapshot(view, options);

    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    sample_batch_t const samples = samples_of(arguments);
    status_t const resolved = resolve_collections(database, collections, arguments.count,
                                                  entry_layout_t::strings_to_uuids_k);
    if (resolved.failed()) return resolved;

    auto [outputs, outputs_status] = strings_scan_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    sample_generator_t generator = sample_generator_t::seeded(&outputs);
    st::vector<std::size_t, arena_allocator<std::size_t>> kept {{view.scratch}};
    iterator_pointer_t iterator {database.native->NewIterator(options)};
    for (std::size_t task_index = 0; task_index != samples.size(); ++task_index) {
        sample_task_t const sample = samples[task_index];
        outputs.open_task(task_index);
        if (!sample.limit) continue;
        ::leveldb::Slice const strings = prefix_of(sample.collection);

        kept.clear();
        std::size_t seen = 0;
        for (iterator->Seek(strings); iterator->Valid() && iterator->key().starts_with(strings);
             iterator->Next(), ++seen) {
            std::size_t const slot = seen < sample.limit ? seen : st::draw_below(generator, seen + 1);
            if (slot >= sample.limit) continue;
            if (slot != kept.size()) kept[slot] = seen;
            else if (status_t const status = growth_status(kept.emplace_back(seen)); status.failed()) return status;
        }
        if (!iterator->status().ok()) return status_of(iterator->status());

        std::sort(kept.begin(), kept.end());
        std::size_t position = 0;
        std::size_t next = 0;
        for (iterator->Seek(strings); iterator->Valid() && next != kept.size(); iterator->Next(), ++position) {
            if (position != kept[next]) continue;
            if (status_t const status = outputs.push(to_value(tail_of(iterator->key()))); status.failed())
                return status;
            ++next;
        }
        if (!iterator->status().ok()) return status_of(iterator->status());
    }
    return outputs.finish(arguments);
}

/** Measures every string range by walking it at the view it reads: its strings, the keys
 *  they map onto as their value bytes, and both with the collection every stored key leads
 *  with as their space. */
status_t strings_measure(ustore_strings_measure_t const& arguments, strings_arg_t const& starts,
                         strings_arg_t const& ends, view_t const& view) {
    if (status_t const status = validate_direction(arguments.direction); status.failed() || !arguments.count)
        return status;

    database_t& database = *state_of<database_t>(arguments.database);
    ::leveldb::ReadOptions options;
    options.fill_cache = false;
    use_snapshot(view, options);

    ustore_scan_direction_t const direction = arguments.direction;
    strided_iterator<ustore_collection_id_t const> const collections = strided(arguments.collections);
    status_t const resolved = resolve_collections(database, collections, arguments.count,
                                                  entry_layout_t::strings_to_uuids_k);
    if (resolved.failed()) return resolved;

    auto [outputs, outputs_status] = measure_outputs_t::make(arguments);
    if (outputs_status.failed()) return outputs_status;
    std::string first;
    iterator_pointer_t iterator {database.native->NewIterator(options)};
    for (std::size_t task = 0; task != arguments.count; ++task) {
        ustore_collection_id_t const collection = collections[task];
        std::string_view const start = starts ? string_of(starts[task]) : std::string_view {};
        std::string_view const end = ends ? string_of(ends[task]) : std::string_view {};
        ::leveldb::Slice const strings = prefix_of(collection);

        ustore_size_t cardinality = 0;
        ustore_size_t string_bytes = 0;
        for (seek_strings(*iterator, collection, {}, start, direction, first);
             iterator->Valid() && iterator->key().starts_with(strings); step_towards(*iterator, direction)) {
            value_view_t const text = to_value(tail_of(iterator->key()));
            if (!string_before_end(text, end, direction)) break;
            ++cardinality;
            string_bytes += text.size();
        }
        if (!iterator->status().ok()) return status_of(iterator->status());
        ustore_size_t const value_bytes = cardinality * sizeof(ustore_key_t);
        outputs.assign(task, cardinality, value_bytes, string_bytes + value_bytes + cardinality * collection_bytes_k);
    }
    outputs.finish(arguments);
    return {};
}

#pragma endregion Strings

} // namespace unum::ustore::oracles::leveldb
