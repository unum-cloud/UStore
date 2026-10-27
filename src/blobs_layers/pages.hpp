/**
 *  @file src/blobs_layers/pages.hpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief The page edits and reads of an engine keeping every page as one value or as its edits, over the reads of
 *      its pages.
 *
 *  Every page reads as one value in its collection, its current shape: the dense element tree of
 *  <tt>ustore/pages/element_tree.hpp</tt> under the page key, so rendering a page is one read and one decode.
 *
 *  An edit call reads the trees of the pages it touches and applies its tasks in order by the forgiving rules of
 *  <tt>page_fold.hpp</tt>, then hands the pages to a lander. An engine keeping every page as one value, served by the
 *  @c *_over_blobs verbs, reads the pages watched in a transaction, writes every tree back in one write, then removes
 *  the pages the call removed. The in-memory engine lands them the same way into its pages stores, inside one
 *  transaction. An engine keeping a page as its edits takes the chains of the @c *_edits calls, which read the pages
 *  unwatched, since the fold lets concurrent edits of one page land, and commits the edits of every page as one
 *  chain. An edit naming a missing page answers @c status_code_t::not_found_k, as a modification of a missing
 *  document does, while an edit of a missing element inside a page does nothing, as the fold does for racing edits.
 *  A Nil element removes the whole page.
 *
 *  Every call places its edits at one stamp, the wall clock in nanoseconds or one past the newest placement in the
 *  pages it touches, whichever is newer, which lands an insertion right after its anchor and prefixes the keys minted
 *  for new elements.
 *
 *  Every scratch structure lives in the scratch of the call, so the arena of the request holds only its outputs. XML
 *  and plain text import and render, while HTML, Markdown and the JSON tree answer @c status_code_t::unsupported_k.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm>   // `std::max`
#include <bit>         // `std::bit_cast`
#include <chrono>      // `std::chrono::system_clock`
#include <limits>      // `std::numeric_limits`
#include <new>         // placement `new`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::underlying_type_t`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/pages.h"    // `ustore_pages_import_t`, `ustore_page_encoding_t`
#include "ustore/ranges.hpp" // `place_batch_t`, `values_view_t`, `sort_and_deduplicate`

#include "ustore/pages/element_tree.hpp"    // `element_tree`, `element_key_t`, `mint_key`
#include "ustore/pages/markup.hpp"          // `import_markup`, `render_markup`, `page_encoding_t`
#include "ustore/pages/page_edits.hpp"      // `edit_t`, `import_edit`, `insert_edit`
#include "ustore/pages/page_fold.hpp"       // `page_state`
#include "ustore/utilities/algorithm.hpp"   // `offset_in_sorted`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`, `arena_allocator`

#include "engine.hpp"             // `blobs_verbs<home_t::host_k>`, `view_t`
#include "lib.hpp"                // `stringzilla_capabilities`
#include "utilities/batches.hpp"  // `strings_arg_t`, `places_of`, `validate_remove`
#include "utilities/outputs.hpp"  // `read_outputs_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`

namespace unum::ustore {
using namespace unum::ustore::utilities;

#pragma region Vocabulary

/** A growable array in the scratch of a call. */
template <typename value_type_>
using scratch_vector = st::basic_vector<value_type_, arena_allocator<value_type_>>;

using page_allocator_t = arena_allocator<byte_t>;
using page_tree_t = pages::element_tree<page_allocator_t>;
using page_state_t = pages::page_state<page_allocator_t>;

static_assert(sizeof(ustore_key_t) == sizeof(pages::element_key_t));

/** The edits one page call made to one page, in task order, as a chain of <tt>ustore/pages/page_edits.hpp</tt>. */
struct page_edits_t {
    ustore_collection_id_t collection;
    ustore_key_t key;
    std::span<byte_t const> chain;
};

/** The element key spelling the same 16 bytes as @p key. */
inline pages::element_key_t element_key_of(ustore_key_t const& key) noexcept {
    pages::element_key_t element;
    std::memcpy(element.bytes, &key, sizeof(element.bytes));
    return element;
}

/** The key of the C ABI spelling the same 16 bytes as @p element. */
inline ustore_key_t key_of(pages::element_key_t const& element) noexcept {
    ustore_key_t key;
    std::memcpy(&key, element.bytes, sizeof(key));
    return key;
}

/** The element key at @p index of @p keys, Nil where they carry no data. */
inline pages::element_key_t task_element(ustore_keys_view_t keys, std::size_t index) noexcept {
    if (!keys.data) return pages::nil_key_k;
    strided_iterator<ustore_key_t const> const column = strided(keys);
    return element_key_of(column[static_cast<std::ptrdiff_t>(index)]);
}

/** The length at @p index of @p lengths, zero where they carry no data. */
inline std::uint64_t task_length(ustore_lengths_view_t lengths, std::size_t index) noexcept {
    if (!lengths.data) return 0;
    strided_iterator<ustore_length_t const> const column = strided(lengths);
    return column[static_cast<std::ptrdiff_t>(index)];
}

/** The bytes of @p value, empty for a missing one. */
inline std::span<byte_t const> bytes_in(value_view_t value) noexcept { return {value.begin(), value.end()}; }

/** The markup encoding @p encoding names, refusing HTML, Markdown and JSON as unsupported for now
 *  and any other value as unknown. */
inline expected<pages::page_encoding_t> markup_encoding(ustore_page_encoding_t const& encoding) noexcept {
    // A C caller may pass any integer, which C++ may not load as the enum, so only the referenced bits are read
    switch (std::bit_cast<std::underlying_type_t<ustore_page_encoding_t>>(encoding)) {
    case ustore_page_encoding_xml_k: return pages::page_encoding_t::xml_k;
    case ustore_page_encoding_text_k: return pages::page_encoding_t::text_k;
    case ustore_page_encoding_html_k:
    case ustore_page_encoding_markdown_k:
    case ustore_page_encoding_json_k:
        return status_t {status_code_t::unsupported_k, "Pages import and render only XML and text for now"};
    default: return status_t {status_code_t::invalid_argument_k, "Unknown page encoding"};
    }
}

/** Nanoseconds since the Unix epoch on the wall clock, where every call's stamp starts. */
inline std::uint64_t clock_stamp() noexcept {
    auto const since_epoch = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count());
}

/** The newest placement stamp of any element of @p tree, zero for an empty tree. */
inline std::uint64_t newest_placement(page_tree_t const& tree) noexcept {
    std::uint64_t newest = 0;
    pages::element_index_t const root = tree.root();
    for (pages::element_index_t at = root; at != pages::no_element_k; at = tree.next_in_preorder(at, root))
        newest = std::max(newest, tree[at].placement.stamp);
    return newest;
}

/** Whether @p edit needs its page to exist: every edit but an import and the removal of a whole page. */
inline bool needs_page(pages::edit_t const& edit) noexcept {
    return edit.modification != pages::modification_t::import_k &&
           !(edit.modification == pages::modification_t::remove_k && edit.element.is_nil());
}

#pragma endregion Vocabulary

#pragma region Touched Pages

/** A page an edit call touches: where it lives, its tree as edited so far, whether it existed
 *  before the call, and the edits the call made to it, in task order. */
struct touched_page_t {
    collection_key_t place;
    page_state_t state;
    bool existed = false;
    scratch_vector<pages::edit_t> edits;

    touched_page_t(collection_key_t place, ustore_arena_t scratch) noexcept
        : place(place), state(page_allocator_t {scratch}), edits(arena_allocator<pages::edit_t> {scratch}) {}
};

/** The distinct pages of an edit call, sorted, and the page of every task. */
struct touched_pages_t {
    std::span<touched_page_t> pages;
    std::span<std::size_t> pages_of_tasks;
};

/** Reads the pages @p places address through @p view with the reads of @p land, each distinct page
 *  once, decoding every tree. */
template <typename lander_type_>
expected<touched_pages_t> read_touched_pages(lander_type_ const& land, place_batch_t const& places,
                                             view_t const& view) noexcept {
    ustore_arena_t const scratch = view.scratch;
    auto [distinct, distinct_status] = arena_allocate<collection_key_t>(scratch, places.count);
    if (distinct_status.failed()) return distinct_status;
    transform_n(places, places.count, distinct.begin(), [](place_t const& place) { return place.collection_key(); });
    std::span<collection_key_t> const sorted = distinct.first(sort_and_deduplicate(distinct.begin(), distinct.end()));
    std::size_t const count = sorted.size();

    auto [pages_of_tasks, mapping_status] = arena_allocate<std::size_t>(scratch, places.count);
    if (mapping_status.failed()) return mapping_status;
    for (std::size_t task = 0; task != places.size(); ++task)
        pages_of_tasks[task] = offset_in_sorted(sorted, places[task].collection_key());

    auto const sorted_strided = strided(sorted).immutable();
    place_batch_t distinct_places;
    distinct_places.collections_begin = sorted_strided.members(&collection_key_t::collection).begin();
    distinct_places.keys_begin = sorted_strided.members(&collection_key_t::key).begin();
    distinct_places.count = static_cast<ustore_size_t>(count);
    auto [found, found_status] = land.read(distinct_places, view);
    if (found_status.failed()) return found_status;

    auto [pages, pages_status] = arena_allocate<touched_page_t>(scratch, count);
    if (pages_status.failed()) return pages_status;
    for (std::size_t index = 0; index != count; ++index) {
        touched_page_t& page = *new (&pages[index]) touched_page_t {sorted[index], scratch};
        value_view_t const tree = found[index];
        page.existed = tree && !tree.empty();
        if (status_t const status = page.state.load(bytes_in(tree)); status.failed()) return status;
    }
    return touched_pages_t {pages, pages_of_tasks};
}

/** Reads the trees of the pages at @p places through @p view with the engine's own @p values_verbs,
 *  appending to its scratch rather than rewinding it. */
inline expected<values_view_t> read_page_values(blobs_verbs<home_t::host_k> const& values_verbs,
                                                ustore_database_t database, place_batch_t const& places,
                                                view_t const& view) noexcept {
    ustore_octet_t const* presences = nullptr;
    ustore_strings_view_t contents {};
    ustore_blobs_read_t read {};
    read.database = database;
    read.count = places.count;
    read.collections = places.collections();
    read.keys = places.keys();
    read.presences = &presences;
    read.values = &contents;
    read.arena = view.scratch;
    if (status_t const status = values_verbs.read(read, view); status.failed()) return status;
    return values_view_t {places.count, presences, contents};
}

/** Writes every tree of @p pages that exists in one write through @p view with the engine's own
 *  @p values_verbs, then removes the pages that stopped existing. */
inline status_t write_touched_pages(blobs_verbs<home_t::host_k> const& values_verbs, ustore_database_t database,
                                    std::span<touched_page_t> pages, ustore_durability_t durability,
                                    ustore_watch_t watch, view_t const& view) noexcept {
    ustore_arena_t const scratch = view.scratch;
    std::size_t const count = pages.size();
    auto [collections, collections_status] = arena_allocate<ustore_collection_id_t>(scratch, count);
    if (collections_status.failed()) return collections_status;
    auto [keys, keys_status] = arena_allocate<ustore_key_t>(scratch, count);
    if (keys_status.failed()) return keys_status;
    auto [values, values_status] = arena_allocate<ustore_bytes_view_t>(scratch, count);
    if (values_status.failed()) return values_status;

    page_allocator_t allocator {scratch};
    std::size_t written = 0, removed = 0;
    for (touched_page_t& page : pages) {
        if (!page.state.exists) {
            removed += page.existed;
            continue;
        }
        auto [tree, tree_status] = page.state.tree.encode(allocator);
        if (tree_status.failed()) return tree_status;
        collections[written] = page.place.collection;
        keys[written] = page.place.key;
        values[written++] = value_view_t {tree}.bytes();
    }

    if (written) {
        ustore_blobs_write_t write {};
        write.database = database;
        write.count = written;
        write.collections = {collections.data(), sizeof(ustore_collection_id_t)};
        write.keys = {keys.data(), sizeof(ustore_key_t)};
        strings_arg_t const written_values = strings_arg_t::from_views(values.data());
        write.values = written_values.view;
        write.durability = durability;
        write.watch = watch;
        if (status_t const status = values_verbs.write(write, written_values, view); status.failed()) return status;
    }
    if (!removed) return {};

    std::size_t listed = 0;
    for (touched_page_t const& page : pages)
        if (page.existed && !page.state.exists)
            collections[listed] = page.place.collection, keys[listed++] = page.place.key;
    ustore_blobs_remove_t remove {};
    remove.database = database;
    remove.count = listed;
    remove.collections = {collections.data(), sizeof(ustore_collection_id_t)};
    remove.keys = {keys.data(), sizeof(ustore_key_t)};
    remove.durability = durability;
    remove.watch = watch;
    return values_verbs.remove(remove, view);
}

/** Lands an edit call on an engine keeping every page as one value, reading the pages watched
 *  through its own @c values_verbs, as a transaction watches the pages it rewrites whatever the
 *  caller asked of the writes, and writing every tree back through them. */
struct rewritten_pages_t {
    static constexpr ustore_watch_t reads_watch_k = ustore_watch_keys_k;

    blobs_verbs<home_t::host_k> const& values_verbs;
    ustore_database_t database;

    /** Reads the trees of the pages at @p places through @p view. */
    expected<values_view_t> read(place_batch_t const& places, view_t const& view) const noexcept {
        return read_page_values(values_verbs, database, places, view);
    }

    template <typename arguments_type_>
    status_t operator()(arguments_type_ const& arguments, std::span<touched_page_t> pages,
                        view_t const& view) const noexcept {
        return write_touched_pages(values_verbs, database, pages, arguments.durability, arguments.watch, view);
    }
};

/** Lands an edit call on an engine keeping a page as its edits, reading the pages unwatched through
 *  its own @c values_verbs and encoding the edits of every page as one chain into @c chains, which
 *  the engine commits. */
struct appended_edits_t {
    static constexpr ustore_watch_t reads_watch_k = ustore_watch_none_k;

    blobs_verbs<home_t::host_k> const& values_verbs;
    ustore_database_t database;

    /** The chain of every page the call touched, in the scratch of the call. */
    std::span<page_edits_t const>& chains;

    /** Reads the trees of the pages at @p places through @p view. */
    expected<values_view_t> read(place_batch_t const& places, view_t const& view) const noexcept {
        return read_page_values(values_verbs, database, places, view);
    }

    template <typename arguments_type_>
    status_t operator()(arguments_type_ const&, std::span<touched_page_t> pages, view_t const& view) const noexcept {
        auto [encoded, encoded_status] = arena_allocate<page_edits_t>(view.scratch, pages.size());
        if (encoded_status.failed()) return encoded_status;
        page_allocator_t allocator {view.scratch};
        for (std::size_t index = 0; index != pages.size(); ++index) {
            touched_page_t const& page = pages[index];
            auto [chain, chain_status] = pages::encode_edits(std::span<pages::edit_t const> {page.edits}, allocator);
            if (chain_status.failed()) return chain_status;
            encoded[index] = page_edits_t {page.place.collection, page.place.key, chain};
        }
        chains = encoded;
        return {};
    }
};

/** Receives a task, its placement and the page it edits, and answers the edit of that task. */
template <typename edit_maker_type_>
concept edit_maker =
    std::is_invocable_r_v<expected<pages::edit_t>, edit_maker_type_&, std::size_t, pages::placement_t, touched_page_t&>;

/**
 *  @brief Applies the edit @p make_edit answers for every task of @p arguments, in order, to the pages they name, read
 *      through @p land, and lands them through @p land.
 *
 *  A lander answers <tt>read(places, view)</tt>, the trees of the pages at @c places as values
 *  appended to the scratch, read as its @c reads_watch_k asks, and lands the edited pages when
 *  called as <tt>land(arguments, pages, view)</tt>. Refuses an edit of a missing page, other than
 *  an import or the removal of a whole page, with @c status_code_t::not_found_k, and writes nothing
 *  on any failure.
 */
template <typename arguments_type_, edit_maker make_edit_type_, typename lander_type_>
status_t edit_pages(arguments_type_ const& arguments, view_t const& view, make_edit_type_&& make_edit,
                    lander_type_ const& land) noexcept {
    if (!arguments.count) return {};
    place_batch_t const places = places_of(arguments);
    if (status_t const status = validate_remove(arguments.transaction, places, arguments.durability, arguments.watch);
        status.failed())
        return status;
    if (arguments.count > std::numeric_limits<std::uint32_t>::max())
        return {status_code_t::invalid_argument_k, "One call edits at most 4 Gi tasks"};

    view_t reading = view;
    reading.watch = lander_type_::reads_watch_k;
    auto [touched, touched_status] = read_touched_pages(land, places, reading);
    if (touched_status.failed()) return touched_status;

    std::uint64_t newest = 0;
    for (touched_page_t const& page : touched.pages) newest = std::max(newest, newest_placement(page.state.tree));
    std::uint64_t const stamp = std::max(clock_stamp(), newest + 1);

    for (std::size_t task = 0; task != arguments.count; ++task) {
        touched_page_t& page = touched.pages[touched.pages_of_tasks[task]];
        pages::placement_t const placement {stamp, static_cast<std::uint32_t>(task)};
        auto [edit, edit_status] = make_edit(task, placement, page);
        if (edit_status.failed()) return edit_status;
        if (!page.state.exists && needs_page(edit))
            return {status_code_t::not_found_k, "The edited page does not exist"};
        if (status_t const status = page.state.apply(edit); status.failed()) return status;
        if (status_t const kept = growth_status(page.edits.push_back(pages::edit_t {edit})); kept.failed()) return kept;
    }
    return land(arguments, touched.pages, view);
}

#pragma endregion Touched Pages

#pragma region Edit Calls

/** Implements @c ustore_pages_import, landing the call through @p land. */
template <typename lander_type_>
status_t serve_import(ustore_pages_import_t const& arguments, strings_arg_t const& values, view_t const& view,
                      lander_type_ const& land) noexcept {
    if (!arguments.count) return {};
    if (!values) return {status_code_t::invalid_argument_k, "No markup was provided"};
    auto [encoding, encoding_status] = markup_encoding(arguments.encoding);
    if (encoding_status.failed()) return encoding_status;

    std::size_t total = 0;
    for (std::size_t task = 0; task != arguments.count; ++task) total += values[task].length;
    progress_t progress {arguments.progress, arguments.progress_state, total};
    page_allocator_t allocator {view.scratch};
    return edit_pages(
        arguments, view,
        [&](std::size_t task, pages::placement_t placement, touched_page_t& page) noexcept -> expected<pages::edit_t> {
            std::uint32_t ordinal = 0;
            auto mint = [&]() noexcept { return pages::mint_key(placement.stamp, placement.sequence, ordinal++); };
            std::string_view const markup = string_of(values[task]);
            pages::element_key_t const root = element_key_of(page.place.key);
            page_tree_t tree {allocator};
            if (status_t const status = pages::import_markup(encoding, markup, root, placement, mint, tree,
                                                             stringzilla_capabilities());
                status.failed())
                return status;
            auto [dense, dense_status] = tree.encode(allocator);
            if (dense_status.failed()) return dense_status;
            if (!progress.advance(markup.size())) return progress_cancelled_k;
            return pages::import_edit(placement, root, dense);
        },
        land);
}

/** Implements @c ustore_pages_insert, landing the call through @p land. */
template <typename lander_type_>
status_t serve_insert(ustore_pages_insert_t const& arguments, strings_arg_t const& values, view_t const& view,
                      lander_type_ const& land) noexcept {
    if (!arguments.count) return {};
    if (!values) return {status_code_t::invalid_argument_k, "No fragments were provided"};
    auto [encoding, encoding_status] = markup_encoding(arguments.encoding);
    if (encoding_status.failed()) return encoding_status;
    auto [effective, effective_status] = arena_allocate<ustore_key_t>(
        arguments.effective_keys ? arguments.arena : view.scratch, arguments.count);
    if (effective_status.failed()) return effective_status;

    page_allocator_t allocator {view.scratch};
    status_t const status = edit_pages(
        arguments, view,
        [&](std::size_t task, pages::placement_t placement, touched_page_t&) noexcept -> expected<pages::edit_t> {
            pages::element_key_t const given = task_element(arguments.elements, task);
            std::uint32_t ordinal = 0;
            bool first = true;
            // The given key lands on the first element of the fragment, and every later one is minted
            auto mint = [&]() noexcept -> pages::element_key_t {
                if (std::exchange(first, false) && !given.is_nil()) return given;
                return pages::mint_key(placement.stamp, placement.sequence, ordinal++);
            };
            page_tree_t fragment {allocator};
            if (status_t const parsed = pages::import_markup(encoding, string_of(values[task]), pages::nil_key_k,
                                                             placement, mint, fragment, stringzilla_capabilities());
                parsed.failed())
                return parsed;
            auto [dense, dense_status] = fragment.encode(allocator);
            if (dense_status.failed()) return dense_status;
            pages::element_index_t const leading = fragment[fragment.root()].first_child;
            pages::element_key_t const landed = leading == pages::no_element_k ? pages::nil_key_k
                                                                               : fragment[leading].key;
            effective[task] = key_of(landed);
            return pages::insert_edit(placement, landed, task_element(arguments.parents, task),
                                      task_element(arguments.anchors, task), dense);
        },
        land);
    if (status.failed()) return status;
    if (arguments.effective_keys) *arguments.effective_keys = effective.data();
    return {};
}

/** Implements @c ustore_pages_remove, landing the call through @p land. */
template <typename lander_type_>
status_t serve_remove(ustore_pages_remove_t const& arguments, view_t const& view, lander_type_ const& land) noexcept {
    if (!arguments.count) return {};
    if (!arguments.elements.data) return {status_code_t::invalid_argument_k, "No elements were provided"};
    return edit_pages(
        arguments, view,
        [&](std::size_t task, pages::placement_t placement, touched_page_t&) noexcept -> expected<pages::edit_t> {
            return pages::remove_edit(placement, task_element(arguments.elements, task));
        },
        land);
}

/** Implements @c ustore_pages_move, landing the call through @p land. */
template <typename lander_type_>
status_t serve_move(ustore_pages_move_t const& arguments, view_t const& view, lander_type_ const& land) noexcept {
    if (!arguments.count) return {};
    if (!arguments.elements.data) return {status_code_t::invalid_argument_k, "No elements were provided"};
    return edit_pages(
        arguments, view,
        [&](std::size_t task, pages::placement_t placement, touched_page_t&) noexcept -> expected<pages::edit_t> {
            return pages::move_edit(placement, task_element(arguments.elements, task),
                                    task_element(arguments.parents, task), task_element(arguments.anchors, task));
        },
        land);
}

/** Implements @c ustore_pages_splice, landing the call through @p land. */
template <typename lander_type_>
status_t serve_splice(ustore_pages_splice_t const& arguments, strings_arg_t const& values, view_t const& view,
                      lander_type_ const& land) noexcept {
    if (!arguments.count) return {};
    if (!arguments.elements.data) return {status_code_t::invalid_argument_k, "No elements were provided"};
    return edit_pages(
        arguments, view,
        [&](std::size_t task, pages::placement_t placement, touched_page_t&) noexcept -> expected<pages::edit_t> {
            std::string_view const bytes = values ? string_of(values[task]) : std::string_view {};
            return pages::splice_edit(placement, task_element(arguments.elements, task),
                                      task_length(arguments.text_starts, task),
                                      task_length(arguments.text_lengths, task), bytes);
        },
        land);
}

/** Implements @c ustore_pages_set_attributes, landing the call through @p land. */
template <typename lander_type_>
status_t serve_set_attributes(ustore_pages_set_attributes_t const& arguments, strings_arg_t const& fields,
                              strings_arg_t const& values, view_t const& view, lander_type_ const& land) noexcept {
    if (!arguments.count) return {};
    if (!arguments.elements.data) return {status_code_t::invalid_argument_k, "No elements were provided"};
    if (!fields || !values) return {status_code_t::invalid_argument_k, "Attributes need their names and values"};
    return edit_pages(
        arguments, view,
        [&](std::size_t task, pages::placement_t placement, touched_page_t&) noexcept -> expected<pages::edit_t> {
            return pages::set_attribute_edit(placement, task_element(arguments.elements, task), string_of(fields[task]),
                                             string_of(values[task]));
        },
        land);
}

/** Implements @c ustore_pages_remove_attributes, landing the call through @p land. */
template <typename lander_type_>
status_t serve_remove_attributes(ustore_pages_remove_attributes_t const& arguments, strings_arg_t const& fields,
                                 view_t const& view, lander_type_ const& land) noexcept {
    if (!arguments.count) return {};
    if (!arguments.elements.data) return {status_code_t::invalid_argument_k, "No elements were provided"};
    if (!fields) return {status_code_t::invalid_argument_k, "No attribute names were provided"};
    return edit_pages(
        arguments, view,
        [&](std::size_t task, pages::placement_t placement, touched_page_t&) noexcept -> expected<pages::edit_t> {
            return pages::remove_attribute_edit(placement, task_element(arguments.elements, task),
                                                string_of(fields[task]));
        },
        land);
}

#pragma endregion Edit Calls

#pragma region Page Calls

/** The edit chains of the page edit calls of an engine keeping a page as its edits, checked and
 *  applied over the pages read unwatched through @p values_verbs, so concurrent edits of one page
 *  both land, leaving the chain of every page the call touched in the scratch of the call for the
 *  engine to commit. */
inline expected<std::span<page_edits_t const>> pages_import_edits(blobs_verbs<home_t::host_k> const& values_verbs,
                                                                  ustore_pages_import_t const& arguments,
                                                                  strings_arg_t const& values,
                                                                  view_t const& view) noexcept {
    std::span<page_edits_t const> chains;
    status_t const status = serve_import(arguments, values, view,
                                         appended_edits_t {values_verbs, arguments.database, chains});
    if (status.failed()) return status;
    return chains;
}

inline expected<std::span<page_edits_t const>> pages_insert_edits(blobs_verbs<home_t::host_k> const& values_verbs,
                                                                  ustore_pages_insert_t const& arguments,
                                                                  strings_arg_t const& values,
                                                                  view_t const& view) noexcept {
    std::span<page_edits_t const> chains;
    status_t const status = serve_insert(arguments, values, view,
                                         appended_edits_t {values_verbs, arguments.database, chains});
    if (status.failed()) return status;
    return chains;
}

inline expected<std::span<page_edits_t const>> pages_remove_edits(blobs_verbs<home_t::host_k> const& values_verbs,
                                                                  ustore_pages_remove_t const& arguments,
                                                                  view_t const& view) noexcept {
    std::span<page_edits_t const> chains;
    status_t const status = serve_remove(arguments, view, appended_edits_t {values_verbs, arguments.database, chains});
    if (status.failed()) return status;
    return chains;
}

inline expected<std::span<page_edits_t const>> pages_move_edits(blobs_verbs<home_t::host_k> const& values_verbs,
                                                                ustore_pages_move_t const& arguments,
                                                                view_t const& view) noexcept {
    std::span<page_edits_t const> chains;
    status_t const status = serve_move(arguments, view, appended_edits_t {values_verbs, arguments.database, chains});
    if (status.failed()) return status;
    return chains;
}

inline expected<std::span<page_edits_t const>> pages_splice_edits(blobs_verbs<home_t::host_k> const& values_verbs,
                                                                  ustore_pages_splice_t const& arguments,
                                                                  strings_arg_t const& values,
                                                                  view_t const& view) noexcept {
    std::span<page_edits_t const> chains;
    status_t const status = serve_splice(arguments, values, view,
                                         appended_edits_t {values_verbs, arguments.database, chains});
    if (status.failed()) return status;
    return chains;
}

inline expected<std::span<page_edits_t const>> pages_set_attributes_edits(
    blobs_verbs<home_t::host_k> const& values_verbs, ustore_pages_set_attributes_t const& arguments,
    strings_arg_t const& fields, strings_arg_t const& values, view_t const& view) noexcept {
    std::span<page_edits_t const> chains;
    status_t const status = serve_set_attributes(arguments, fields, values, view,
                                                 appended_edits_t {values_verbs, arguments.database, chains});
    if (status.failed()) return status;
    return chains;
}

inline expected<std::span<page_edits_t const>> pages_remove_attributes_edits(
    blobs_verbs<home_t::host_k> const& values_verbs, ustore_pages_remove_attributes_t const& arguments,
    strings_arg_t const& fields, view_t const& view) noexcept {
    std::span<page_edits_t const> chains;
    status_t const status = serve_remove_attributes(arguments, fields, view,
                                                    appended_edits_t {values_verbs, arguments.database, chains});
    if (status.failed()) return status;
    return chains;
}

/** Implements @c ustore_pages_read over the trees @p read_trees answers, called as
 *  <tt>read_trees()</tt> for the pages the call names, task by task, as values appended to the
 *  scratch. */
template <typename read_trees_type_>
status_t read_pages(ustore_pages_read_t const& arguments, view_t const& view, read_trees_type_&& read_trees) noexcept {
    if (!arguments.count) return {};
    if (!arguments.keys.data) return {status_code_t::invalid_argument_k, "No keys were provided"};
    bool const renders = arguments.values || arguments.lengths;
    pages::page_encoding_t encoding = pages::page_encoding_t::xml_k;
    if (renders) {
        auto [chosen, encoding_status] = markup_encoding(arguments.outputs_encoding);
        if (encoding_status.failed()) return encoding_status;
        encoding = chosen;
    }
    auto [found, found_status] = read_trees();
    if (found_status.failed()) return found_status;

    auto [outputs, outputs_status] = read_outputs_t::make(arguments, view.scratch);
    if (outputs_status.failed()) return outputs_status;
    page_allocator_t allocator {view.scratch};
    page_tree_t tree {allocator};
    place_batch_t const places = places_of(arguments);
    for (std::size_t task = 0; task != arguments.count; ++task) {
        value_view_t const page = found[task];
        if (!page || page.empty()) {
            outputs.append_missing();
            continue;
        }
        // Consecutive tasks of one page decode it once
        if (!task || !(places[task].collection_key() == places[task - 1].collection_key()) || tree.empty())
            if (status_t const status = tree.decode(bytes_in(page)); status.failed()) return status;
        pages::element_key_t const wanted = task_element(arguments.elements, task);
        pages::element_index_t const element = wanted.is_nil() ? tree.root() : tree.find(wanted);
        if (element == pages::no_element_k) {
            outputs.append_missing();
            continue;
        }
        if (!renders) {
            outputs.append(value_view_t::make_empty());
            continue;
        }
        scratch_vector<char> rendered {arena_allocator<char> {view.scratch}};
        if (status_t const status = pages::render_markup(tree, element, encoding, rendered); status.failed())
            return status;
        outputs.append(std::string_view(rendered.begin(), rendered.end()));
    }
    return outputs.finish(arguments);
}

#pragma endregion Page Calls

#pragma region Pages over Blobs

/** The page verbs of an engine keeping every page as one value, its dense element tree, bound to
 *  its unchecked whole-value verbs @p values_verbs_ and its resolver @p resolve_, which refuses
 *  every collection holding another modality first. */
template <blobs_verbs<home_t::host_k> const& values_verbs_, auto resolve_>
status_t pages_import_over_blobs(ustore_pages_import_t const& arguments, strings_arg_t const& values,
                                 view_t const& view) noexcept {
    status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count,
                                       ustore_modality_pages_k, view);
    if (resolved.failed()) return resolved;
    return serve_import(arguments, values, view, rewritten_pages_t {values_verbs_, arguments.database});
}

template <blobs_verbs<home_t::host_k> const& values_verbs_, auto resolve_>
status_t pages_insert_over_blobs(ustore_pages_insert_t const& arguments, strings_arg_t const& values,
                                 view_t const& view) noexcept {
    status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count,
                                       ustore_modality_pages_k, view);
    if (resolved.failed()) return resolved;
    return serve_insert(arguments, values, view, rewritten_pages_t {values_verbs_, arguments.database});
}

template <blobs_verbs<home_t::host_k> const& values_verbs_, auto resolve_>
status_t pages_remove_over_blobs(ustore_pages_remove_t const& arguments, view_t const& view) noexcept {
    status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count,
                                       ustore_modality_pages_k, view);
    if (resolved.failed()) return resolved;
    return serve_remove(arguments, view, rewritten_pages_t {values_verbs_, arguments.database});
}

template <blobs_verbs<home_t::host_k> const& values_verbs_, auto resolve_>
status_t pages_move_over_blobs(ustore_pages_move_t const& arguments, view_t const& view) noexcept {
    status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count,
                                       ustore_modality_pages_k, view);
    if (resolved.failed()) return resolved;
    return serve_move(arguments, view, rewritten_pages_t {values_verbs_, arguments.database});
}

template <blobs_verbs<home_t::host_k> const& values_verbs_, auto resolve_>
status_t pages_splice_over_blobs(ustore_pages_splice_t const& arguments, strings_arg_t const& values,
                                 view_t const& view) noexcept {
    status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count,
                                       ustore_modality_pages_k, view);
    if (resolved.failed()) return resolved;
    return serve_splice(arguments, values, view, rewritten_pages_t {values_verbs_, arguments.database});
}

template <blobs_verbs<home_t::host_k> const& values_verbs_, auto resolve_>
status_t pages_set_attributes_over_blobs(ustore_pages_set_attributes_t const& arguments, strings_arg_t const& fields,
                                         strings_arg_t const& values, view_t const& view) noexcept {
    status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count,
                                       ustore_modality_pages_k, view);
    if (resolved.failed()) return resolved;
    return serve_set_attributes(arguments, fields, values, view, rewritten_pages_t {values_verbs_, arguments.database});
}

template <blobs_verbs<home_t::host_k> const& values_verbs_, auto resolve_>
status_t pages_remove_attributes_over_blobs(ustore_pages_remove_attributes_t const& arguments,
                                            strings_arg_t const& fields, view_t const& view) noexcept {
    status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count,
                                       ustore_modality_pages_k, view);
    if (resolved.failed()) return resolved;
    return serve_remove_attributes(arguments, fields, view, rewritten_pages_t {values_verbs_, arguments.database});
}

template <blobs_verbs<home_t::host_k> const& values_verbs_, auto resolve_>
status_t pages_read_over_blobs(ustore_pages_read_t const& arguments, view_t const& view) noexcept {
    status_t const resolved = resolve_(arguments.database, arguments.collections, arguments.count,
                                       ustore_modality_pages_k, view);
    if (resolved.failed()) return resolved;
    return read_pages(arguments, view, [&]() noexcept {
        return read_page_values(values_verbs_, arguments.database, places_of(arguments), view);
    });
}

#pragma endregion Pages over Blobs

} // namespace unum::ustore
