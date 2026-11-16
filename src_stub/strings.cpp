/**
 *  @file src_stub/strings.cpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The strings calls over the store of @c lib.hpp, whose collections key every entry by its
 *      text and map it onto one @c ustore_key_t.
 */
#include "lib.hpp"

using namespace unum::ustore::stub;

namespace unum::ustore::stub {

/** The strings one task names: those starting with @c prefix, from @c start, inclusive, towards
 *  @c end, exclusive, in @c direction, where an empty start begins at the first string in the
 *  direction and an empty end bounds nothing. */
struct strings_window_t {
    std::string prefix;
    std::string start;
    std::string end;
    ustore_scan_direction_t direction = ustore_scan_ascending_k;
};

/** The least string greater than every string starting with @p prefix, or empty when none is. */
inline std::string prefix_successor(std::string prefix) {
    while (!prefix.empty() && static_cast<unsigned char>(prefix.back()) == 0xFF) prefix.pop_back();
    if (!prefix.empty()) prefix.back() = static_cast<char>(static_cast<unsigned char>(prefix.back()) + 1);
    return prefix;
}

inline bool window_accepts(std::string const& key, void const* context) noexcept {
    return std::string_view(key).starts_with(static_cast<strings_window_t const*>(context)->prefix);
}

inline bool window_stops(std::string const& key, void const* context) noexcept {
    auto const& window = *static_cast<strings_window_t const*>(context);
    if (window.direction == ustore_scan_descending_k) return key < window.prefix;
    return !std::string_view(key).starts_with(window.prefix) && key > window.prefix;
}

/** The walk over @p window, whose @c accept and @c stop read the window it points at. */
inline walk_t strings_walk(strings_window_t const& window) {
    walk_t walk;
    walk.direction = window.direction;
    walk.accept = &window_accepts;
    walk.stop = &window_stops;
    walk.context = &window;
    if (!window.end.empty()) walk.end = window.end, walk.bounded = true;
    if (window.direction == ustore_scan_ascending_k) {
        std::string const& from = larger_of(window.start, window.prefix);
        if (!from.empty()) walk.position = from, walk.kind = bound_t::inclusive_k;
        return walk;
    }
    if (!window.start.empty()) walk.position = window.start, walk.kind = bound_t::inclusive_k;
    else if (!window.prefix.empty()) {
        std::string const successor = prefix_successor(window.prefix);
        if (!successor.empty()) walk.position = successor, walk.kind = bound_t::exclusive_k;
    }
    return walk;
}

/** The key a predicate sees for a string: the key it maps onto. */
inline ustore_key_t mapped_key(entry_t const& entry) noexcept { return key_of(entry.value); }

/** The text patterns match for a string: the string itself. */
inline std::string_view string_text(entry_t const& entry) noexcept { return entry.key; }

inline sieve_t strings_sieve(ustore_predicate_t const& predicate, matcher_t const& matcher) {
    sieve_t sieve;
    sieve.predicate = predicate;
    sieve.matcher = matcher.everything ? nullptr : &matcher;
    sieve.key_for = &mapped_key;
    sieve.text_for = &string_text;
    return sieve;
}

/** Reads the view and resolves the strings collections of a listing under a shared lock. */
template <typename arguments_type_>
bool open_strings_listing(call_t& call, arguments_type_ const& arguments, view_spec_t& spec,
                          std::vector<collection_t const*>& resolved) {
    if (!decode_view(call, arguments.view, arguments.database, spec) || refuse_staged_watch(call, spec)) return false;
    name_view(call, spec);
    std::shared_lock lock(arguments.database->mutex);
    reader_t const reader = reader_of(arguments.database, spec, arguments.database->head);
    return resolve_collections(call, reader, arguments.collections, arguments.count, ustore_modality_strings_k,
                               resolved);
}

/** Reads an optional ragged input of @p count strings, where a zeroed view gives an empty
 *  string per task. */
inline bool read_optional_strings(call_t& call, ustore_strings_view_t const& view, std::size_t count,
                                  strings_in_t& strings) {
    if (!read_strings(call, view, count, strings)) return false;
    if (!strings.given) strings.items.assign(count, {});
    return true;
}

} // namespace unum::ustore::stub

/** What @c ustore_strings_cursor_t names: a copy of its window and the string its next
 *  pull starts at. */
struct ustore_strings_cursor_handle_t {
    magic_t magic = magic_t::cursor_k;
    ustore_database_handle_t* database = nullptr;
    view_spec_t spec;
    std::string collection;
    strings_window_t window;
    walk_t walk;
    matcher_t matcher;
    ustore_predicate_t predicate {};

    /** Whether a page came back short, which ends the walk for good. */
    bool exhausted = false;
    claim_t claim;
    named_calls_t named_calls {0};
};

void ustore_strings_write(ustore_strings_write_t* arguments) {
    if (submitted<&ustore_strings_write>(arguments)) return;
    call_t call(arguments->outcome);
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count;
    strings_in_t strings;
    if (refuse_collections(call, arguments->collections, count)) return;
    if (!read_strings(call, arguments->strings, count, strings)) return;
    if (count && (!strings.given || !arguments->values.data))
        return call.refuse(ustore_status_invalid_argument_k, "The call needs its strings and values");
    if (refuse_write_choices(call, arguments->transaction, arguments->durability, arguments->watch) ||
        refuse_write_transaction(call, arguments->transaction, arguments->database) ||
        !pre_resolve(call, arguments->database, arguments->transaction, arguments->collections, count,
                     ustore_modality_strings_k) ||
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
                ustore_key_t const& value = strided_element(arguments->values.data, arguments->values.stride, index);
                writer.put(*collection, std::string(strings[index]), bytes_of(value));
            }
        });
}

void ustore_strings_remove(ustore_strings_remove_t* arguments) {
    if (submitted<&ustore_strings_remove>(arguments)) return;
    call_t call(arguments->outcome);
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count;
    strings_in_t strings, ends;
    if (refuse_collections(call, arguments->collections, count)) return;
    if (!read_strings(call, arguments->strings, count, strings) ||
        !read_strings(call, arguments->end_strings, count, ends))
        return;
    if (count && !strings.given) return call.refuse(ustore_status_invalid_argument_k, "No strings were provided");
    if (refuse_write_choices(call, arguments->transaction, arguments->durability, arguments->watch) ||
        refuse_write_transaction(call, arguments->transaction, arguments->database) ||
        !pre_resolve(call, arguments->database, arguments->transaction, arguments->collections, count,
                     ustore_modality_strings_k) ||
        refuse_read_only(call, arguments->database))
        return;
    if (!call.hold_executor(arguments->executor) || !hold_transaction(call, arguments->transaction)) return;
    write_through(call, arguments->database, arguments->transaction, arguments->watch, arguments->durability,
                  [&](reader_t const& reader, writer_t& writer) {
                      for (std::size_t index = 0; index != count; ++index) {
                          collection_t const* const collection = reader.find(bytes_of(
                              strided_element(arguments->collections.data, arguments->collections.stride, index)));
                          if (!collection)
                              return call.refuse(ustore_status_not_found_k, "A collection was dropped meanwhile");
                          if (!ends.given) {
                              writer.remove(*collection, std::string(strings[index]));
                              continue;
                          }
                          strings_window_t window;
                          window.start = strings[index], window.end = ends[index];
                          walk_t walk = strings_walk(window);
                          if (window.start.empty()) walk.kind = bound_t::none_k;
                          record_walk(reader, *collection, walk);
                          std::vector<entry_t> listed;
                          fetch(reader, *collection, walk, static_cast<std::size_t>(-1), listed);
                          for (entry_t const& entry : listed) writer.remove(*collection, entry.key);
                      }
                  });
}

void ustore_strings_read(ustore_strings_read_t* arguments) {
    if (submitted<&ustore_strings_read>(arguments)) return;
    call_t call(arguments->outcome);
    if (arguments->presences) *arguments->presences = nullptr;
    if (arguments->values) *arguments->values = nullptr;
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count;
    strings_in_t strings;
    if (!arguments->arena) return call.refuse(ustore_status_invalid_argument_k, "The call needs an arena");
    if (refuse_collections(call, arguments->collections, count)) return;
    if (!read_strings(call, arguments->strings, count, strings)) return;
    if (count && !strings.given) return call.refuse(ustore_status_invalid_argument_k, "No strings were provided");
    view_spec_t spec;
    std::vector<collection_t const*> resolved;
    if (!open_strings_listing(call, *arguments, spec, resolved)) return;
    if (!call.hold_executor(arguments->executor)) return;
    ustore_arena_handle_t* const arena = claim_arena(
        call, arguments->arena,
        {arguments->collections.data, first_input(arguments->strings), data_input(arguments->strings)});
    if (!arena || !hold_view(call, spec)) return;
    std::vector<ustore_key_t> values(count);
    std::vector<bool> present(count, false);
    {
        std::shared_lock lock(arguments->database->mutex);
        reader_t const reader = reader_of(arguments->database, spec, arguments->database->head);
        for (std::size_t index = 0; index != count; ++index) {
            collection_t const* const collection = reader.find(bytes_of(resolved[index]->id));
            if (!collection) return call.refuse(ustore_status_not_found_k, "A collection was dropped meanwhile");
            std::string const* const value = reader.get(*collection, std::string(strings[index]));
            if (value) values[index] = key_of(*value), present[index] = true;
        }
    }
    auto* const presences = arguments->presences ? carve<ustore_octet_t>(call, *arena, (count + 7) / 8) : nullptr;
    auto* const keys = arguments->values ? carve<ustore_key_t>(call, *arena, count) : nullptr;
    if (call.failed()) return;
    for (std::size_t index = 0; index != count; ++index) {
        if (presences && present[index]) set_bit(presences, index);
        if (keys) keys[index] = values[index];
    }
    if (arguments->presences) *arguments->presences = presences;
    if (arguments->values) *arguments->values = keys;
}

void ustore_strings_scan(ustore_strings_scan_t* arguments) {
    if (submitted<&ustore_strings_scan>(arguments)) return;
    call_t call(arguments->outcome);
    if (arguments->offsets) *arguments->offsets = nullptr;
    if (arguments->strings) *arguments->strings = {};
    if (arguments->scores) *arguments->scores = nullptr;
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count;
    if (!arguments->arena) return call.refuse(ustore_status_invalid_argument_k, "The call needs an arena");
    if (refuse_collections(call, arguments->collections, count)) return;
    if (!arguments->offsets || !arguments->strings)
        return call.refuse(ustore_status_invalid_argument_k, "Need outputs for the offsets and the strings");
    strings_in_t prefixes, starts, ends;
    if (!read_optional_strings(call, arguments->prefixes, count, prefixes) ||
        !read_optional_strings(call, arguments->start_strings, count, starts) ||
        !read_optional_strings(call, arguments->end_strings, count, ends))
        return;
    if (refuse_direction(call, arguments->direction) || refuse_predicate(call, arguments->predicate)) return;
    matcher_t matcher;
    if (!compile_patterns(call, arguments->patterns, matcher)) return;
    if ((!matcher.everything || arguments->predicate.callback) && refuse_mode(call, arguments->mode)) return;
    view_spec_t spec;
    std::vector<collection_t const*> resolved;
    if (!open_strings_listing(call, *arguments, spec, resolved)) return;
    if (!call.hold_executor(arguments->executor)) return;
    ustore_arena_handle_t* const arena = claim_arena(
        call, arguments->arena,
        {arguments->collections.data, first_input(arguments->prefixes), first_input(arguments->start_strings),
         first_input(arguments->end_strings)});
    if (!arena || !hold_view(call, spec)) return;
    read_pin_t pin;
    reader_t const reader = reader_of(arguments->database, spec,
                                      spec.snapshot || spec.transaction ? 0 : pin.pin(arguments->database));
    sieve_t const sieve = strings_sieve(arguments->predicate, matcher);
    std::vector<entry_t> listed;
    std::vector<float> scores;
    std::vector<ustore_length_t> offsets(count + 1, 0);
    try {
        for (std::size_t task = 0; task != count; ++task) {
            strings_window_t window;
            window.prefix = prefixes[task], window.start = starts[task], window.end = ends[task];
            window.direction = arguments->direction;
            walk_t walk = strings_walk(window);
            if (!list_entries(reader, bytes_of(resolved[task]->id), walk, sieve, listed.size() + arguments->wanted,
                              listed, scores))
                return call.refuse(ustore_status_not_found_k, "A collection was dropped meanwhile");
            offsets[task + 1] = listed.size();
            if (!carries_on(arguments->progress, arguments->progress_state, task + 1, count))
                return call.refuse(ustore_status_cancelled_k, "The progress callback cancelled the scan");
        }
    }
    catch (std::bad_alloc const&) {
        return call.refuse(ustore_status_out_of_memory_k, "Out of memory while scanning");
    }
    auto* const offsets_out = carve<ustore_length_t>(call, *arena, count + 1);
    float* const scores_out = arguments->scores && matcher.scored() ? carve<float>(call, *arena, listed.size())
                                                                    : nullptr;
    std::vector<std::string_view> texts;
    for (entry_t const& entry : listed) texts.push_back(entry.key);
    ustore_strings_view_t tape {};
    if (!texts.empty() && !write_tape(call, *arena, texts, tape)) return;
    if (call.failed()) return;
    std::copy(offsets.begin(), offsets.end(), offsets_out);
    if (scores_out) std::copy(scores.begin(), scores.end(), scores_out);
    *arguments->offsets = offsets_out;
    *arguments->strings = tape;
    if (arguments->scores) *arguments->scores = scores_out;
}

void ustore_strings_cursor_init(ustore_strings_cursor_init_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->cursor) *arguments->cursor = nullptr;
    if (refuse_database(call, arguments->database)) return;
    if (!arguments->cursor)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the cursor");
    if (refuse_direction(call, arguments->direction) || refuse_predicate(call, arguments->predicate)) return;
    auto* const cursor = new (std::nothrow) ustore_strings_cursor_handle_t {};
    if (!cursor) return call.refuse(ustore_status_out_of_memory_k, "Failed to allocate the cursor");
    if (!compile_patterns(call, arguments->patterns, cursor->matcher) ||
        !decode_view(call, arguments->view, arguments->database, cursor->spec))
        return delete cursor;
    cursor->database = arguments->database;
    cursor->collection = bytes_of(arguments->collection);
    cursor->window.prefix = text_of(arguments->prefix);
    cursor->window.start = text_of(arguments->start_string);
    cursor->window.end = text_of(arguments->end_string);
    cursor->window.direction = arguments->direction;
    cursor->walk = strings_walk(cursor->window);
    cursor->predicate = arguments->predicate;
    arguments->database->holds.fetch_add(1, std::memory_order_acq_rel);
    if (cursor->spec.snapshot) cursor->spec.snapshot->holds.fetch_add(1, std::memory_order_acq_rel);
    if (cursor->spec.transaction) cursor->spec.transaction->holds.fetch_add(1, std::memory_order_acq_rel);
    *arguments->cursor = cursor;
}

void ustore_strings_cursor_next(ustore_strings_cursor_next_t* arguments) {
    if (submitted<&ustore_strings_cursor_next>(arguments)) return;
    call_t call(arguments->outcome);
    if (arguments->strings) *arguments->strings = {};
    if (arguments->values) *arguments->values = nullptr;
    if (arguments->written) *arguments->written = 0;
    if (refuse_database(call, arguments->database)) return;
    ustore_strings_cursor_handle_t* const cursor = arguments->cursor;
    if (!cursor || !arguments->arena || !arguments->strings || !arguments->written)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs a cursor, an arena and outputs");
    if (cursor->database != arguments->database)
        return call.refuse(ustore_status_invalid_argument_k, "The cursor walks another database");
    if (cursor->spec.transaction && cursor->spec.transaction->staged && records_reads(cursor->spec))
        return call.refuse(ustore_status_invalid_argument_k, "A watching pull goes through a staged transaction");
    call.name(&cursor->named_calls);
    if (!call.hold_executor(arguments->executor)) return;
    ustore_arena_handle_t* const arena = claim_arena(call, arguments->arena);
    if (!arena || !call.claim(&cursor->claim, "Another call pulls the cursor") || !hold_view(call, cursor->spec))
        return;
    {
        std::shared_lock lock(cursor->database->mutex);
        reader_t const reader = reader_of(cursor->database, cursor->spec, cursor->database->head);
        collection_t const* const collection = reader.find(cursor->collection);
        if (!collection) return call.refuse(ustore_status_not_found_k, "The collection does not exist");
        if (collection->modality != ustore_modality_strings_k)
            return call.refuse(ustore_status_invalid_argument_k, "The collection holds another modality");
    }
    read_pin_t pin;
    reader_t const reader = reader_of(
        cursor->database, cursor->spec,
        cursor->spec.snapshot || cursor->spec.transaction ? 0 : pin.pin(cursor->database));
    walk_t walk = cursor->walk;
    walk.context = &cursor->window;
    std::vector<entry_t> listed;
    std::vector<float> scores;
    try {
        if (!cursor->exhausted &&
            !list_entries(reader, cursor->collection, walk, strings_sieve(cursor->predicate, cursor->matcher),
                          arguments->wanted, listed, scores))
            return call.refuse(ustore_status_not_found_k, "The collection was dropped meanwhile");
    }
    catch (std::bad_alloc const&) {
        return call.refuse(ustore_status_out_of_memory_k, "Out of memory while pulling");
    }
    if (!carries_on(arguments->progress, arguments->progress_state, 1, 1))
        return call.refuse(ustore_status_cancelled_k, "The progress callback cancelled the pull");
    std::vector<std::string_view> texts;
    for (entry_t const& entry : listed) texts.push_back(entry.key);
    ustore_strings_view_t tape {};
    if (!texts.empty() && !write_tape(call, *arena, texts, tape)) return;
    auto* const keys = arguments->values ? carve<ustore_key_t>(call, *arena, listed.size()) : nullptr;
    if (call.failed()) return;
    for (std::size_t index = 0; keys && index != listed.size(); ++index) keys[index] = mapped_key(listed[index]);
    if (!listed.empty()) cursor->walk.position = listed.back().key, cursor->walk.kind = bound_t::exclusive_k;
    cursor->exhausted = listed.size() < arguments->wanted;
    *arguments->strings = tape;
    if (arguments->values) *arguments->values = keys;
    *arguments->written = listed.size();
}

void ustore_strings_cursor_free(ustore_strings_cursor_t cursor) {
    if (!cursor) return;
    wait_named(cursor->named_calls);
    while (cursor->claim.held()) std::this_thread::yield();
    if (cursor->spec.snapshot) release_snapshot(cursor->spec.snapshot);
    if (cursor->spec.transaction) release_transaction(cursor->spec.transaction);
    release_database(cursor->database);
    delete cursor;
}

void ustore_strings_sample(ustore_strings_sample_t* arguments) {
    if (submitted<&ustore_strings_sample>(arguments)) return;
    call_t call(arguments->outcome);
    if (arguments->offsets) *arguments->offsets = nullptr;
    if (arguments->strings) *arguments->strings = {};
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count;
    if (!arguments->arena) return call.refuse(ustore_status_invalid_argument_k, "The call needs an arena");
    if (refuse_collections(call, arguments->collections, count)) return;
    if (!arguments->offsets || !arguments->strings)
        return call.refuse(ustore_status_invalid_argument_k, "Need outputs for the offsets and the strings");
    view_spec_t spec;
    std::vector<collection_t const*> resolved;
    if (!open_strings_listing(call, *arguments, spec, resolved)) return;
    if (!call.hold_executor(arguments->executor)) return;
    ustore_arena_handle_t* const arena = claim_arena(call, arguments->arena, {arguments->collections.data});
    if (!arena || !hold_view(call, spec)) return;
    std::vector<std::string> drawn;
    std::vector<ustore_length_t> offsets(count + 1, 0);
    try {
        std::shared_lock lock(arguments->database->mutex);
        reader_t const reader = reader_of(arguments->database, spec, arguments->database->head);
        for (std::size_t task = 0; task != count; ++task) {
            collection_t const* const collection = reader.find(bytes_of(resolved[task]->id));
            if (!collection) return call.refuse(ustore_status_not_found_k, "A collection was dropped meanwhile");
            walk_t walk;
            std::vector<entry_t> population;
            fetch(reader, *collection, walk, static_cast<std::size_t>(-1), population);
            std::vector<std::string> texts;
            for (entry_t& entry : population) texts.push_back(std::move(entry.key));
            for (std::string& text : draw(std::move(texts), arguments->wanted)) drawn.push_back(std::move(text));
            offsets[task + 1] = drawn.size();
        }
    }
    catch (std::bad_alloc const&) {
        return call.refuse(ustore_status_out_of_memory_k, "Out of memory while sampling");
    }
    auto* const offsets_out = carve<ustore_length_t>(call, *arena, count + 1);
    ustore_strings_view_t tape {};
    if (!drawn.empty() && !write_tape(call, *arena, drawn, tape)) return;
    if (call.failed()) return;
    std::copy(offsets.begin(), offsets.end(), offsets_out);
    *arguments->offsets = offsets_out;
    *arguments->strings = tape;
}

void ustore_strings_measure(ustore_strings_measure_t* arguments) {
    if (submitted<&ustore_strings_measure>(arguments)) return;
    call_t call(arguments->outcome);
    ustore_size_t const** const outputs[] = {arguments->min_cardinalities, arguments->max_cardinalities,
                                             arguments->min_value_bytes,   arguments->max_value_bytes,
                                             arguments->min_space_usages,  arguments->max_space_usages};
    for (ustore_size_t const** output : outputs)
        if (output) *output = nullptr;
    if (refuse_database(call, arguments->database)) return;
    std::size_t const count = arguments->count;
    if (!arguments->arena) return call.refuse(ustore_status_invalid_argument_k, "The call needs an arena");
    if (refuse_collections(call, arguments->collections, count)) return;
    strings_in_t starts, ends;
    if (!read_optional_strings(call, arguments->start_strings, count, starts) ||
        !read_optional_strings(call, arguments->end_strings, count, ends) ||
        refuse_direction(call, arguments->direction))
        return;
    view_spec_t spec;
    std::vector<collection_t const*> resolved;
    if (!open_strings_listing(call, *arguments, spec, resolved)) return;
    if (!call.hold_executor(arguments->executor)) return;
    ustore_arena_handle_t* const arena = claim_arena(call, arguments->arena, {arguments->collections.data});
    if (!arena || !hold_view(call, spec)) return;
    // The values of strings are their keys, the least they take, and the strings themselves add
    // to the most
    std::vector<ustore_size_t> cardinalities(count, 0), key_bytes(count, 0), bytes(count, 0);
    try {
        std::shared_lock lock(arguments->database->mutex);
        reader_t const reader = reader_of(arguments->database, spec, arguments->database->head);
        for (std::size_t task = 0; task != count; ++task) {
            collection_t const* const collection = reader.find(bytes_of(resolved[task]->id));
            if (!collection) return call.refuse(ustore_status_not_found_k, "A collection was dropped meanwhile");
            strings_window_t window;
            window.start = starts[task], window.end = ends[task], window.direction = arguments->direction;
            walk_t walk = strings_walk(window);
            std::vector<entry_t> listed;
            fetch(reader, *collection, walk, static_cast<std::size_t>(-1), listed);
            cardinalities[task] = listed.size();
            for (entry_t const& entry : listed)
                key_bytes[task] += entry.value.size(), bytes[task] += entry.key.size() + entry.value.size();
        }
    }
    catch (std::bad_alloc const&) {
        return call.refuse(ustore_status_out_of_memory_k, "Out of memory while measuring");
    }
    std::vector<ustore_size_t> const* const sources[] = {&cardinalities, &cardinalities, &key_bytes,
                                                         &bytes,         &key_bytes,     &bytes};
    ustore_size_t* carved[6] = {};
    for (std::size_t index = 0; index != 6; ++index)
        if (outputs[index]) carved[index] = carve<ustore_size_t>(call, *arena, count);
    if (call.failed()) return;
    for (std::size_t index = 0; index != 6; ++index)
        if (outputs[index]) {
            if (count) std::copy(sources[index]->begin(), sources[index]->end(), carved[index]);
            *outputs[index] = carved[index];
        }
}
