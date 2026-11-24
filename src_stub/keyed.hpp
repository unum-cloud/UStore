/**
 *  @file src_stub/keyed.hpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief The listings every collection keyed by @c ustore_key_t shares: scans, samples, measures
 *      and cursors over blobs, documents, pages and the vertices of graphs.
 *
 *  The four modalities differ only in what a walk lists, a graph listing only the entries recording
 *  its vertices, and in the extra verdict a walk asks, a documents or pages filter, so each verb
 *  here takes the modality and that verdict, and reads the fields every argument structure of the
 *  family names alike.
 */
#ifndef UNUM_USTORE_STUB_KEYED_HPP
#define UNUM_USTORE_STUB_KEYED_HPP

#include <memory> // `std::shared_ptr` - a compiled filter a cursor keeps

#include "lib.hpp"

namespace unum::ustore::stub {

/** The extra verdict a walk asks of every entry its patterns kept, and the state
 *  it compiled once. */
struct verdict_t {
    bool (*keeps)(entry_t const& entry, void const* state) = nullptr;
    std::shared_ptr<void const> state;
};

/** Whether the graph entry at @p key records a vertex, the only entries a walk
 *  of vertices lists. */
inline bool is_vertex_entry(std::string const& key, void const*) noexcept {
    return key.size() == half_edge_bytes_k && key[16] == 0;
}

/** The key every entry of a keyed collection names: its first 16 bytes, a
 *  graph's vertex included. */
inline ustore_key_t entry_key(entry_t const& entry) noexcept { return key_of(entry.key); }

/** The text patterns match for a keyed entry: its value. */
inline std::string_view entry_value(entry_t const& entry) noexcept { return entry.value; }

/** The entry a walk of @p modality starts or ends at for @p key. */
inline std::string bound_entry(ustore_modality_t modality, ustore_key_t const& key) {
    return modality == ustore_modality_graph_k ? vertex_entry(key) : bytes_of(key);
}

/** A walk over the keys of a collection of @p modality, from @p start, inclusive, towards
 *  @p end, exclusive. */
inline walk_t keyed_walk(ustore_modality_t modality, ustore_key_t const* start, ustore_key_t const* end,
                         ustore_scan_direction_t direction) {
    walk_t walk;
    walk.direction = direction;
    if (start) walk.position = bound_entry(modality, *start), walk.kind = bound_t::inclusive_k;
    if (end) walk.end = bound_entry(modality, *end), walk.bounded = true;
    if (modality == ustore_modality_graph_k) walk.accept = &is_vertex_entry;
    return walk;
}

/** The sieve of a keyed walk through @p matcher and @p verdict. */
inline sieve_t keyed_sieve(ustore_predicate_t const& predicate, matcher_t const* matcher, verdict_t const& verdict) {
    sieve_t sieve;
    sieve.predicate = predicate;
    sieve.matcher = matcher && !matcher->everything ? matcher : nullptr;
    sieve.key_for = &entry_key;
    sieve.text_for = &entry_value;
    sieve.keeps = verdict.keeps;
    sieve.keeps_context = verdict.state.get();
    return sieve;
}

/** Decodes, checks and resolves the view and collections every keyed listing reads, under a
 *  shared lock, into @p spec and @p resolved. */
template <typename arguments_type_>
bool open_listing(call_t& call, arguments_type_ const& arguments, ustore_modality_t modality, view_spec_t& spec,
                  std::vector<collection_t const*>& resolved) {
    if (!decode_view(call, arguments.view, arguments.database, spec) || refuse_staged_watch(call, spec)) return false;
    name_view(call, spec);
    std::shared_lock lock(arguments.database->mutex);
    reader_t const reader = reader_of(arguments.database, spec, arguments.database->head);
    return resolve_collections(call, reader, arguments.collections, arguments.count, modality, resolved);
}

/** Implements the scans of keyed collections: up to @c wanted keys of every range, through the
 *  predicate, the patterns a blobs scan holds, and @p verdict, a documents or pages filter. */
template <typename arguments_type_>
void scan_keyed(call_t& call, arguments_type_* arguments, ustore_modality_t modality, matcher_t const* matcher,
                verdict_t const& verdict) {
    std::size_t const count = arguments->count;
    view_spec_t spec;
    std::vector<collection_t const*> resolved;
    if (!open_listing(call, *arguments, modality, spec, resolved)) return;
    if (!call.hold_executor(arguments->executor)) return;
    ustore_arena_handle_t* const arena = claim_arena(
        call, arguments->arena, {arguments->collections.data, arguments->start_keys.data, arguments->end_keys.data});
    if (!arena || !hold_view(call, spec)) return;

    read_pin_t pin;
    reader_t const reader = reader_of(arguments->database, spec,
                                      spec.snapshot || spec.transaction ? 0 : pin.pin(arguments->database));
    sieve_t const sieve = keyed_sieve(arguments->predicate, matcher, verdict);
    std::vector<entry_t> listed;
    std::vector<float> scores;
    std::vector<ustore_length_t> offsets(count + 1, 0);
    try {
        for (std::size_t task = 0; task != count; ++task) {
            ustore_key_t const* const start = arguments->start_keys.data
                                                  ? &strided_element(arguments->start_keys.data,
                                                                     arguments->start_keys.stride, task)
                                                  : nullptr;
            ustore_key_t const* const end = arguments->end_keys.data
                                                ? &strided_element(arguments->end_keys.data, arguments->end_keys.stride,
                                                                   task)
                                                : nullptr;
            walk_t walk = keyed_walk(modality, start, end, arguments->direction);
            std::size_t const before = listed.size();
            if (!list_entries(reader, bytes_of(resolved[task]->id), walk, sieve, before + arguments->wanted, listed,
                              scores))
                return call.refuse(ustore_status_not_found_k, "A collection was dropped meanwhile");
            offsets[task + 1] = listed.size();
            if (!keeps_going(arguments->progress, arguments->progress_state, task + 1, count))
                return call.refuse(ustore_status_cancelled_k, "The progress callback cancelled the scan");
        }
    }
    catch (std::bad_alloc const&) {
        return call.refuse(ustore_status_out_of_memory_k, "Out of memory while scanning");
    }
    auto* const offsets_out = carve<ustore_length_t>(call, *arena, count + 1);
    auto* const keys_out = carve<ustore_key_t>(call, *arena, listed.size());
    float* scores_out = nullptr;
    if constexpr (scores_matches_concept<arguments_type_>)
        if (arguments->scores && matcher && matcher->scored()) scores_out = carve<float>(call, *arena, listed.size());
    if (call.failed()) return;
    std::copy(offsets.begin(), offsets.end(), offsets_out);
    for (std::size_t index = 0; index != listed.size(); ++index) {
        keys_out[index] = entry_key(listed[index]);
        if (scores_out) scores_out[index] = scores[index];
    }
    *arguments->offsets = offsets_out;
    *arguments->keys = keys_out;
    if constexpr (scores_matches_concept<arguments_type_>)
        if (arguments->scores) *arguments->scores = scores_out;
}

/** Checks what every keyed scan shares before its filter compiles: the arena, the collections,
 *  the outputs, the mode, the direction and the predicate's cost. */
template <typename arguments_type_>
bool check_scan(call_t& call, arguments_type_* arguments) noexcept {
    if (arguments->offsets) *arguments->offsets = nullptr;
    if (arguments->keys) *arguments->keys = nullptr;
    if constexpr (scores_matches_concept<arguments_type_>)
        if (arguments->scores) *arguments->scores = nullptr;
    if (refuse_database(call, arguments->database)) return false;
    if (!arguments->arena) return call.fail(ustore_status_invalid_argument_k, "The call needs an arena");
    if (refuse_collections(call, arguments->collections, arguments->count)) return false;
    if (!arguments->offsets || !arguments->keys)
        return call.fail(ustore_status_invalid_argument_k, "Need outputs for the offsets and the keys");
    return !refuse_mode(call, arguments->mode) && !refuse_direction(call, arguments->direction) &&
           !refuse_predicate(call, arguments->predicate);
}

/** Implements the samples of keyed collections: up to @c wanted keys of every
 *  collection, drawn uniformly. */
template <typename arguments_type_>
void sample_keyed(arguments_type_* arguments, ustore_modality_t modality) {
    call_t call(arguments->outcome);
    if (arguments->offsets) *arguments->offsets = nullptr;
    if (arguments->keys) *arguments->keys = nullptr;
    if (refuse_database(call, arguments->database)) return;
    if (!arguments->arena) return call.refuse(ustore_status_invalid_argument_k, "The call needs an arena");
    if (refuse_collections(call, arguments->collections, arguments->count)) return;
    if (!arguments->offsets || !arguments->keys)
        return call.refuse(ustore_status_invalid_argument_k, "Need outputs for the offsets and the keys");
    std::size_t const count = arguments->count;
    view_spec_t spec;
    std::vector<collection_t const*> resolved;
    if (!open_listing(call, *arguments, modality, spec, resolved)) return;
    if (!call.hold_executor(arguments->executor)) return;
    ustore_arena_handle_t* const arena = claim_arena(call, arguments->arena, {arguments->collections.data});
    if (!arena || !hold_view(call, spec)) return;
    std::vector<ustore_key_t> drawn;
    std::vector<ustore_length_t> offsets(count + 1, 0);
    try {
        std::shared_lock lock(arguments->database->mutex);
        reader_t const reader = reader_of(arguments->database, spec, arguments->database->head);
        for (std::size_t task = 0; task != count; ++task) {
            collection_t const* const collection = reader.find(bytes_of(resolved[task]->id));
            if (!collection) return call.refuse(ustore_status_not_found_k, "A collection was dropped meanwhile");
            walk_t walk = keyed_walk(modality, nullptr, nullptr, ustore_scan_ascending_k);
            std::vector<entry_t> population;
            fetch(reader, *collection, walk, static_cast<std::size_t>(-1), population);
            std::vector<ustore_key_t> keys;
            keys.reserve(population.size());
            for (entry_t const& entry : population) keys.push_back(entry_key(entry));
            for (ustore_key_t const& key : draw(std::move(keys), arguments->wanted)) drawn.push_back(key);
            offsets[task + 1] = drawn.size();
        }
    }
    catch (std::bad_alloc const&) {
        return call.refuse(ustore_status_out_of_memory_k, "Out of memory while sampling");
    }
    auto* const offsets_out = carve<ustore_length_t>(call, *arena, count + 1);
    auto* const keys_out = carve<ustore_key_t>(call, *arena, drawn.size());
    if (call.failed()) return;
    std::copy(offsets.begin(), offsets.end(), offsets_out);
    std::copy(drawn.begin(), drawn.end(), keys_out);
    *arguments->offsets = offsets_out;
    *arguments->keys = keys_out;
}

/** Implements the measures of keyed collections: exact counts and bytes of every range
 *  at the view. */
template <typename arguments_type_>
void measure_keyed(arguments_type_* arguments, ustore_modality_t modality) {
    call_t call(arguments->outcome);
    ustore_size_t const** const outputs[] = {arguments->min_cardinalities, arguments->max_cardinalities,
                                             arguments->min_value_bytes,   arguments->max_value_bytes,
                                             arguments->min_space_usages,  arguments->max_space_usages};
    for (ustore_size_t const** output : outputs)
        if (output) *output = nullptr;
    if (refuse_database(call, arguments->database)) return;
    if (!arguments->arena) return call.refuse(ustore_status_invalid_argument_k, "The call needs an arena");
    if (refuse_collections(call, arguments->collections, arguments->count) ||
        refuse_direction(call, arguments->direction))
        return;
    std::size_t const count = arguments->count;
    view_spec_t spec;
    std::vector<collection_t const*> resolved;
    if (!open_listing(call, *arguments, modality, spec, resolved)) return;
    if (!call.hold_executor(arguments->executor)) return;
    ustore_arena_handle_t* const arena = claim_arena(
        call, arguments->arena, {arguments->collections.data, arguments->start_keys.data, arguments->end_keys.data});
    if (!arena || !hold_view(call, spec)) return;
    std::vector<ustore_size_t> cardinalities(count, 0), value_bytes(count, 0), space(count, 0);
    try {
        std::shared_lock lock(arguments->database->mutex);
        reader_t const reader = reader_of(arguments->database, spec, arguments->database->head);
        for (std::size_t task = 0; task != count; ++task) {
            collection_t const* const collection = reader.find(bytes_of(resolved[task]->id));
            if (!collection) return call.refuse(ustore_status_not_found_k, "A collection was dropped meanwhile");
            ustore_key_t const* const start = arguments->start_keys.data
                                                  ? &strided_element(arguments->start_keys.data,
                                                                     arguments->start_keys.stride, task)
                                                  : nullptr;
            ustore_key_t const* const end = arguments->end_keys.data
                                                ? &strided_element(arguments->end_keys.data, arguments->end_keys.stride,
                                                                   task)
                                                : nullptr;
            walk_t walk = keyed_walk(modality, start, end, arguments->direction);
            std::vector<entry_t> listed;
            fetch(reader, *collection, walk, static_cast<std::size_t>(-1), listed);
            cardinalities[task] = listed.size();
            for (entry_t const& entry : listed) {
                value_bytes[task] += entry.value.size();
                space[task] += entry.key.size() + entry.value.size();
            }
        }
    }
    catch (std::bad_alloc const&) {
        return call.refuse(ustore_status_out_of_memory_k, "Out of memory while measuring");
    }
    std::vector<ustore_size_t> const* const sources[] = {&cardinalities, &cardinalities, &value_bytes,
                                                         &value_bytes,   &space,         &space};
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

/** Stages the removal of every key of @p count tasks, or of every key in its range when
 *  @p end_keys are given, from collections of @p modality keyed by @c ustore_key_t. */
inline void remove_keyed(call_t& call, reader_t const& reader, writer_t& writer, ustore_collections_view_t collections,
                         ustore_keys_view_t keys, ustore_keys_view_t end_keys, std::size_t count,
                         ustore_modality_t modality) {
    for (std::size_t index = 0; index != count; ++index) {
        collection_t const* const collection = reader.find(
            bytes_of(strided_element(collections.data, collections.stride, index)));
        if (!collection) return call.refuse(ustore_status_not_found_k, "A collection was dropped meanwhile");
        ustore_key_t const& key = strided_element(keys.data, keys.stride, index);
        if (!end_keys.data) {
            writer.remove(*collection, bytes_of(key));
            continue;
        }
        ustore_key_t const& end = strided_element(end_keys.data, end_keys.stride, index);
        walk_t walk = keyed_walk(modality, &key, &end, ustore_scan_ascending_k);
        record_walk(reader, *collection, walk);
        std::vector<entry_t> listed;
        fetch(reader, *collection, walk, static_cast<std::size_t>(-1), listed);
        for (entry_t const& entry : listed) writer.remove(*collection, entry.key);
    }
}

/** Writes the presences, lengths and tape of a read of @p values into @p arena and
 *  the outputs given. */
inline void write_values(call_t& call, ustore_arena_handle_t& arena, std::vector<std::string> const& values,
                         std::vector<bool> const& present, ustore_octet_t const** presences_out,
                         ustore_length_t const** lengths_out, ustore_strings_view_t* values_out) {
    std::size_t const count = values.size();
    auto* const presences = presences_out ? carve<ustore_octet_t>(call, arena, (count + 7) / 8) : nullptr;
    auto* const lengths = lengths_out ? carve<ustore_length_t>(call, arena, count) : nullptr;
    ustore_strings_view_t tape {};
    if (values_out && count && !write_tape(call, arena, values, tape)) return;
    if (call.failed()) return;
    for (std::size_t index = 0; index != count; ++index) {
        if (presences && present[index]) set_bit(presences, index);
        if (lengths) lengths[index] = values[index].size();
    }
    if (presences_out) *presences_out = presences;
    if (lengths_out) *lengths_out = lengths;
    if (values_out) *values_out = tape;
}

/** The walk a keyed cursor keeps between pulls, and what it reads through. */
struct keyed_cursor_t {
    magic_t magic = magic_t::cursor_k;
    ustore_database_handle_t* database = nullptr;
    view_spec_t spec;
    std::string collection;
    ustore_modality_t modality = ustore_modality_blobs_k;
    walk_t walk;
    matcher_t matcher;
    ustore_predicate_t predicate {};
    verdict_t verdict;

    /** Whether a page came back short, which ends the walk for good. */
    bool exhausted = false;
    claim_t claim;
    named_calls_t named_calls {0};
};

/** Opens a keyed cursor into @p cursor from @p arguments, holding its database and the parent
 *  its view names. */
template <typename arguments_type_>
bool open_keyed_cursor(call_t& call, arguments_type_ const* arguments, ustore_modality_t modality,
                       keyed_cursor_t& cursor) {
    if (!decode_view(call, arguments->view, arguments->database, cursor.spec)) return false;
    cursor.database = arguments->database;
    cursor.collection = bytes_of(arguments->collection);
    cursor.modality = modality;
    cursor.walk = keyed_walk(modality, &arguments->start_key, arguments->end_key, arguments->direction);
    cursor.predicate = arguments->predicate;
    arguments->database->holds.fetch_add(1, std::memory_order_acq_rel);
    if (cursor.spec.snapshot) cursor.spec.snapshot->holds.fetch_add(1, std::memory_order_acq_rel);
    if (cursor.spec.transaction) cursor.spec.transaction->holds.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

/** Gives back the holds a keyed cursor kept, once no call pulls it. */
inline void close_keyed_cursor(keyed_cursor_t& cursor) noexcept {
    wait_named(cursor.named_calls);
    while (cursor.claim.held()) std::this_thread::yield();
    if (cursor.spec.snapshot) release_snapshot(cursor.spec.snapshot);
    if (cursor.spec.transaction) release_transaction(cursor.spec.transaction);
    release_database(cursor.database);
}

/** Pulls the next page of a keyed cursor into @p listed, leaving the cursor where it was when
 *  the pull fails or is cancelled; @p call has checked the outputs and holds the arena. */
template <typename arguments_type_>
bool pull_keyed(call_t& call, arguments_type_ const* arguments, keyed_cursor_t& cursor, std::vector<entry_t>& listed) {
    if (cursor.spec.transaction && cursor.spec.transaction->staged && records_reads(cursor.spec))
        return call.fail(ustore_status_invalid_argument_k, "A watching pull goes through a staged transaction");
    {
        std::shared_lock lock(cursor.database->mutex);
        reader_t const reader = reader_of(cursor.database, cursor.spec, cursor.database->head);
        auto const found = cursor.database->collections.find(cursor.collection);
        if (found == cursor.database->collections.end() || !reader.find(cursor.collection))
            return call.fail(ustore_status_not_found_k, "The collection does not exist or was dropped");
        if (found->second.modality != cursor.modality)
            return call.fail(ustore_status_invalid_argument_k, "The collection holds another modality");
    }
    if (!hold_view(call, cursor.spec)) return false;
    if (cursor.exhausted)
        return keeps_going(arguments->progress, arguments->progress_state, 1, 1) ||
               call.fail(ustore_status_cancelled_k, "The progress callback cancelled the pull");
    read_pin_t pin;
    reader_t const reader = reader_of(cursor.database, cursor.spec,
                                      cursor.spec.snapshot || cursor.spec.transaction ? 0 : pin.pin(cursor.database));
    walk_t walk = cursor.walk;
    std::vector<float> scores;
    try {
        if (!list_entries(reader, cursor.collection, walk,
                          keyed_sieve(cursor.predicate, &cursor.matcher, cursor.verdict), arguments->wanted, listed,
                          scores))
            return call.fail(ustore_status_not_found_k, "The collection was dropped meanwhile");
    }
    catch (std::bad_alloc const&) {
        return call.fail(ustore_status_out_of_memory_k, "Out of memory while pulling");
    }
    if (!keeps_going(arguments->progress, arguments->progress_state, 1, 1))
        return call.fail(ustore_status_cancelled_k, "The progress callback cancelled the pull");
    // A page of fewer than wanted exhausts the walk, and a full one resumes past its last key
    if (!listed.empty()) cursor.walk.position = listed.back().key, cursor.walk.kind = bound_t::exclusive_k;
    cursor.exhausted = listed.size() < arguments->wanted;
    return true;
}

/** Checks what every keyed cursor pull shares: the database, the cursor, the arena and the
 *  outputs, then holds the executor, the arena and the cursor. */
template <typename arguments_type_>
ustore_arena_handle_t* open_pull(call_t& call, arguments_type_* arguments, keyed_cursor_t* cursor) {
    if (arguments->keys) *arguments->keys = nullptr;
    if (arguments->written) *arguments->written = 0;
    if constexpr (exports_values_concept<arguments_type_>)
        if (arguments->values) *arguments->values = {};
    if (refuse_database(call, arguments->database)) return nullptr;
    if (!cursor || !arguments->arena || !arguments->written || !arguments->keys) {
        call.refuse(ustore_status_invalid_argument_k, "The call needs a cursor, an arena and outputs");
        return nullptr;
    }
    if (cursor->database != arguments->database) {
        call.refuse(ustore_status_invalid_argument_k, "The cursor walks another database");
        return nullptr;
    }
    call.name(&cursor->named_calls);
    if (!call.hold_executor(arguments->executor)) return nullptr;
    ustore_arena_handle_t* const arena = claim_arena(call, arguments->arena);
    if (!arena || !call.claim(&cursor->claim, "Another call pulls the cursor")) return nullptr;
    return arena;
}

/** Writes the keys of a pulled page into @p arena and the outputs of @p arguments. */
template <typename arguments_type_>
bool write_page_keys(call_t& call, arguments_type_* arguments, ustore_arena_handle_t& arena,
                     std::vector<entry_t> const& listed) {
    auto* const keys = carve<ustore_key_t>(call, arena, listed.size());
    if (call.failed()) return false;
    for (std::size_t index = 0; index != listed.size(); ++index) keys[index] = entry_key(listed[index]);
    *arguments->keys = keys;
    *arguments->written = listed.size();
    return true;
}

} // namespace unum::ustore::stub

#endif // UNUM_USTORE_STUB_KEYED_HPP
