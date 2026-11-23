/**
 *  @file src_stub/blobs.cpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The blobs calls over the store of @c lib.hpp: writes, removals, reads, scans, cursors,
 *      samples and measures of binary values.
 */
#include "keyed.hpp"

using namespace unum::ustore::stub;

/** What @c ustore_blobs_cursor_t names. */
struct ustore_blobs_cursor_handle_t {
    keyed_cursor_t cursor;
};

void ustore_blobs_write(ustore_blobs_write_t* arguments) {
    call_t call(arguments->outcome);
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count;
    strings_in_t values;
    if (refuse_collections(call, arguments->collections, count)) return;
    if (refuse_write_keys(call, arguments->keys, count)) return;
    if (!read_strings(call, arguments->values, count, values)) return;
    if (count && !values.given) return call.refuse(ustore_status_invalid_argument_k, "No values were provided");
    if (refuse_write_choices(call, arguments->transaction, arguments->durability, arguments->watch) ||
        refuse_write_transaction(call, arguments->transaction, arguments->database) ||
        !pre_resolve(call, arguments->database, arguments->transaction, arguments->collections, count,
                     ustore_modality_blobs_k) ||
        refuse_read_only(call, arguments->database))
        return;
    if (!call.hold_executor(arguments->executor) || !hold_transaction(call, arguments->transaction)) return;
    write_through(
        call, arguments->database, arguments->transaction, arguments->watch, arguments->durability,
        [&](reader_t const& reader, writer_t& writer) {
            for (std::size_t index = 0; index != count; ++index) {
                collection_t const* const collection = reader.find(
                    bytes_of(strided_element(arguments->collections.data, arguments->collections.stride, index)));
                if (!collection) return call.refuse(ustore_status_not_found_k, "A collection was dropped meanwhile");
                ustore_key_t const& key = strided_element(arguments->keys.data, arguments->keys.stride, index);
                writer.put(*collection, bytes_of(key), std::string(values[index]));
            }
        });
}

void ustore_blobs_remove(ustore_blobs_remove_t* arguments) {
    call_t call(arguments->outcome);
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count;
    if (refuse_collections(call, arguments->collections, count)) return;
    if (refuse_write_keys(call, arguments->keys, count)) return;
    if (refuse_write_choices(call, arguments->transaction, arguments->durability, arguments->watch) ||
        refuse_write_transaction(call, arguments->transaction, arguments->database) ||
        !pre_resolve(call, arguments->database, arguments->transaction, arguments->collections, count,
                     ustore_modality_blobs_k) ||
        refuse_read_only(call, arguments->database))
        return;
    if (!call.hold_executor(arguments->executor) || !hold_transaction(call, arguments->transaction)) return;
    write_through(call, arguments->database, arguments->transaction, arguments->watch, arguments->durability,
                  [&](reader_t const& reader, writer_t& writer) {
                      remove_keyed(call, reader, writer, arguments->collections, arguments->keys, arguments->end_keys,
                                   count, ustore_modality_blobs_k);
                  });
}

void ustore_blobs_read(ustore_blobs_read_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->presences) *arguments->presences = nullptr;
    if (arguments->lengths) *arguments->lengths = nullptr;
    if (arguments->values) *arguments->values = {};
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count;
    if (!arguments->arena) return call.refuse(ustore_status_invalid_argument_k, "The call needs an arena");
    if (refuse_collections(call, arguments->collections, count)) return;
    if (count && !arguments->keys.data) return call.refuse(ustore_status_invalid_argument_k, "No keys were provided");
    view_spec_t spec;
    std::vector<collection_t const*> resolved;
    if (!open_listing(call, *arguments, ustore_modality_blobs_k, spec, resolved)) return;
    if (!call.hold_executor(arguments->executor)) return;
    ustore_arena_handle_t* const arena = claim_arena(
        call, arguments->arena,
        {arguments->collections.data, arguments->keys.data, arguments->offsets.data, arguments->limits.data});
    if (!arena || !hold_view(call, spec)) return;
    std::vector<std::string> values(count);
    std::vector<bool> present(count, false);
    try {
        std::shared_lock lock(arguments->database->mutex);
        reader_t const reader = reader_of(arguments->database, spec, arguments->database->head);
        for (std::size_t index = 0; index != count; ++index) {
            collection_t const* const collection = reader.find(bytes_of(resolved[index]->id));
            if (!collection) return call.refuse(ustore_status_not_found_k, "A collection was dropped meanwhile");
            ustore_key_t const& key = strided_element(arguments->keys.data, arguments->keys.stride, index);
            std::string const* const value = reader.get(*collection, bytes_of(key));
            if (!value) continue;
            present[index] = true;
            std::size_t const offset = arguments->offsets.data
                                           ? strided_element(arguments->offsets.data, arguments->offsets.stride, index)
                                           : 0;
            std::size_t const begin = smaller_of(offset, value->size());
            std::size_t length = value->size() - begin;
            if (arguments->limits.data)
                length = smaller_of<std::size_t>(
                    length, strided_element(arguments->limits.data, arguments->limits.stride, index));
            if (arguments->values || arguments->lengths) values[index].assign(*value, begin, length);
        }
    }
    catch (std::bad_alloc const&) {
        return call.refuse(ustore_status_out_of_memory_k, "Out of memory while reading");
    }
    write_values(call, *arena, values, present, arguments->presences, arguments->lengths, arguments->values);
}

void ustore_blobs_scan(ustore_blobs_scan_t* arguments) {
    call_t call(arguments->outcome);
    if (!check_scan(call, arguments)) return;
    matcher_t matcher;
    if (!compile_patterns(call, arguments->patterns, matcher)) return;
    scan_keyed(call, arguments, ustore_modality_blobs_k, &matcher, {});
}

void ustore_blobs_cursor_init(ustore_blobs_cursor_init_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->cursor) *arguments->cursor = nullptr;
    if (refuse_database(call, arguments->database)) return;
    if (!arguments->cursor)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the cursor");
    if (refuse_direction(call, arguments->direction) || refuse_predicate(call, arguments->predicate)) return;
    auto* const cursor = new (std::nothrow) ustore_blobs_cursor_handle_t {};
    if (!cursor) return call.refuse(ustore_status_out_of_memory_k, "Failed to allocate the cursor");
    if (!compile_patterns(call, arguments->patterns, cursor->cursor.matcher) ||
        !open_keyed_cursor(call, arguments, ustore_modality_blobs_k, cursor->cursor))
        return delete cursor;
    *arguments->cursor = cursor;
}

void ustore_blobs_cursor_next(ustore_blobs_cursor_next_t* arguments) {
    call_t call(arguments->outcome);
    keyed_cursor_t* const cursor = arguments->cursor ? &arguments->cursor->cursor : nullptr;
    ustore_arena_handle_t* const arena = open_pull(call, arguments, cursor);
    if (!arena) return;
    std::vector<entry_t> listed;
    if (!pull_keyed(call, arguments, *cursor, listed)) return;
    ustore_strings_view_t values {};
    if (arguments->values && !listed.empty()) {
        std::vector<std::string_view> texts;
        for (entry_t const& entry : listed) texts.push_back(entry.value);
        if (!write_tape(call, *arena, texts, values)) return;
    }
    if (!write_page_keys(call, arguments, *arena, listed)) return;
    if (arguments->values) *arguments->values = values;
}

void ustore_blobs_cursor_free(ustore_blobs_cursor_t cursor) {
    if (!cursor) return;
    close_keyed_cursor(cursor->cursor);
    delete cursor;
}

void ustore_blobs_sample(ustore_blobs_sample_t* arguments) { sample_keyed(arguments, ustore_modality_blobs_k); }

void ustore_blobs_measure(ustore_blobs_measure_t* arguments) { measure_keyed(arguments, ustore_modality_blobs_k); }
