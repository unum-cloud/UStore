/**
 *  @file cpp/ustore/pages_collection.hpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief Handles to one collection of pages, ordered element trees edited by position, whose verbs
 *      block or answer deferred calls as their receiver chose.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <iterator> // `std::size`
#include <span>     // `std::span`

#include "ustore/blobs_range.hpp" // `blobs_range_t`, `keys_range_t`, `size_estimates_t`, `sample_keys_`, `measure_keys_`
#include "ustore/pages.h"
#include "ustore/ranges.hpp" // `keys_view_t`, `values_view_t`, `strided_span`, `strings_batch_concept`
#include "ustore/status.hpp" // `status_t`, `answer`, `called_`, `refused_`, `defers`
#include "ustore/types.hpp"  // `arena_t`, `receiver_t`, `collection_access_t`, `progress_arg_t`, `collection_drop_`

namespace unum::ustore {

/** The view a column of a batch of @p count tasks passes as: zeroed when it is empty, for the call
 *  to default it or refuse it, and the column itself when it holds one item per task. */
template <typename view_type_, typename element_type_>
status_t pages_column_(strided_span<element_type_ const> column, std::size_t count, view_type_& view) noexcept {
    if (column.empty()) view = {};
    else if (column.size() == count) view = {column.data(), column.stride()};
    else return status_t {status_code_t::invalid_argument_k, "Every column of a batch holds one item per task"};
    return {};
}

/** The view a column of strings of a batch of @p count tasks passes as, like @c pages_column_. */
template <strings_batch_concept strings_type_>
status_t pages_strings_(strings_type_ const& strings, std::size_t count, ustore_strings_view_t& view) noexcept {
    std::size_t const given = std::size(strings);
    if (given == 0) view = {};
    else if (given == count) view = strings_view_of(strings);
    else return status_t {status_code_t::invalid_argument_k, "Every column of a batch holds one item per task"};
    return {};
}

/**
 *  @brief A handle to one collection of pages, cheap to construct and to copy, which addresses
 *      the head or the view of a transaction or snapshot, and whose verbs answer as
 *      @p completion_ says.
 *
 *  A page is an ordered tree of elements addressed by key, where the page key doubles as the key of
 *  its root, and every edit names its page, then its element, parent or anchor, one task each.
 *  A blocking handle answers an @c outcome, while a deferred one answers a @c deferred call,
 *  borrowing the handle, its inputs and its arena until the call is final. A handle with
 *  @c collection_access_t::read_only_k only reads, as a snapshot hands out.
 */
template <ustore_completion_t completion_, collection_access_t access_>
class pages_collection_on {
    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {};

    static constexpr bool blocks_k = !defers<completion_>;
    static constexpr bool writes_k = access_ == collection_access_t::read_write_k;

  public:
    /** The handle a snapshot hands out for this kind of collection, which only reads. */
    using read_only_t = pages_collection_on<completion_, collection_access_t::read_only_k>;

    /** The same kind of handle, completing its verbs as @p other_completion_ says. */
    template <ustore_completion_t other_completion_>
    using completed_as = pages_collection_on<other_completion_, access_>;

    /** What the collections this kind of handle addresses hold, which a creation records and a
     *  lookup checks. */
    static constexpr ustore_modality_t modality_k = ustore_modality_pages_k;

    pages_collection_on() noexcept = default;
    pages_collection_on(receiver_t receiver, ustore_collection_id_t collection) noexcept
        : receiver_(receiver), collection_(collection) {}

    operator ustore_collection_id_t() const noexcept { return collection_; }
    ustore_collection_id_t* member_ptr() noexcept { return &collection_; }
    receiver_t receiver() const noexcept { return receiver_; }

    /** The same collection, whose verbs block on @p executor, or on the calling
     *  thread without one. */
    completed_as<ustore_completion_blocking_k> on(ustore_executor_t executor) const noexcept {
        return {receiver_.on(executor), collection_};
    }

    /** The same collection, whose verbs submit to @p executor and answer deferred calls. */
    completed_as<ustore_completion_deferred_k> deferred(ustore_executor_t executor) const noexcept {
        return {receiver_.on(executor), collection_};
    }

#pragma region Verbs

    /**
     *  @brief Replaces the pages of @p keys pairwise with the parsed @p markups, minting a new
     *      key for every element.
     *  @param[in] keys Keys of the pages, borrowed until the call is final.
     *  @param[in] markups One markup per page, borrowed until the call is final.
     *  @param[in] encoding Encoding of the markups.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     *  @param[in] progress Hears the bytes parsed and may cancel the call, borrowed until the
     *      call is final.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k The @p keys and @p markups differ in count.
     */
    template <strings_batch_concept markups_type_>
    answer<completion_, void> import(keys_view_t keys, markups_type_ const& markups,
                                     ustore_page_encoding_t encoding = ustore_page_encoding_xml_k,
                                     ustore_durability_t durability = ustore_durability_buffered_k,
                                     progress_arg_t progress = {}) const noexcept
        requires writes_k
    {
        // The call has one count, so only this side sees the keys and the markups disagree
        if (keys.size() != std::size(markups))
            return refused_<completion_, void>({status_code_t::invalid_argument_k, "Every page needs its own markup"});
        return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            ustore_pages_import_t arguments {};
            arguments.database = receiver_.database;
            arguments.executor = receiver_.executor;
            arguments.outcome = outcome;
            arguments.completion = completion_;
            arguments.transaction = receiver_.transaction;
            arguments.count = keys.size();
            arguments.collections = {&collection_, 0};
            arguments.keys = {keys.data(), keys.stride()};
            arguments.values = strings_view_of(markups);
            arguments.encoding = encoding;
            arguments.durability = durability;
            arguments.progress = progress.callback;
            arguments.progress_state = progress.state;
            ustore_pages_import(&arguments);
        });
    }

    /**
     *  @brief Parses one fragment per task into elements under a parent after an anchor of the page
     *      @p keys names, answering the key every first element ended up with, given or minted.
     *  @param[in] keys Keys of the pages, borrowed until the call is final.
     *  @param[in] fragments One fragment per task, borrowed until the call is final.
     *  @param[in] arena Memory the keys land in, which the answer views, borrowed until the
     *      call is final.
     *  @param[in] parents The element every fragment lands under, or none for the root in every
     *      task, borrowed until the call is final.
     *  @param[in] anchors The sibling every fragment lands after, or none for the front in every
     *      task, borrowed until the call is final.
     *  @param[in] elements The key of the first element of every fragment, or none to mint them,
     *      borrowed until the call is final.
     *  @param[in] encoding Encoding of the fragments.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k A column holds neither none nor one item per key.
     */
    template <strings_batch_concept fragments_type_>
    answer<completion_, keys_view_t> insert(
        keys_view_t keys, fragments_type_ const& fragments, arena_t& arena, keys_view_t parents = {},
        keys_view_t anchors = {}, keys_view_t elements = {},
        ustore_page_encoding_t encoding = ustore_page_encoding_xml_k,
        ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        std::size_t const count = keys.size();
        ustore_keys_view_t parents_view {}, anchors_view {}, elements_view {};
        ustore_strings_view_t fragments_view {};
        status_t shaped = pages_strings_(fragments, count, fragments_view);
        if (shaped.succeeded()) shaped = pages_column_(parents, count, parents_view);
        if (shaped.succeeded()) shaped = pages_column_(anchors, count, anchors_view);
        if (shaped.succeeded()) shaped = pages_column_(elements, count, elements_view);
        if (shaped.failed()) return refused_<completion_, keys_view_t>(shaped);
        return called_<completion_, keys_view_t>(
            [&](ustore_outcome_t* outcome, outputs<keys_view_t>* written) noexcept {
                written->count = count;
                ustore_pages_insert_t insert {};
                insert.database = receiver_.database;
                insert.executor = receiver_.executor;
                insert.outcome = outcome;
                insert.completion = completion_;
                insert.transaction = receiver_.transaction;
                insert.count = count;
                insert.collections = {&collection_, 0};
                insert.keys = {keys.data(), keys.stride()};
                insert.elements = elements_view;
                insert.parents = parents_view;
                insert.anchors = anchors_view;
                insert.values = fragments_view;
                insert.encoding = encoding;
                insert.effective_keys = &written->keys;
                insert.arena = arena;
                insert.durability = durability;
                ustore_pages_insert(&insert);
            });
    }

    /**
     *  @brief Removes one element per task with its subtree, or every page from each of @p keys to
     *      its entry of @p end_keys when they are given.
     *  @param[in] keys Keys of the pages, or the inclusive starts of the ranges, borrowed until the
     *      call is final.
     *  @param[in] elements Elements to remove, none when @p end_keys are given, borrowed until the
     *      call is final.
     *  @param[in] end_keys Exclusive ends of the ranges of whole pages, or none to remove
     *      @p elements alone, borrowed until the call is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k A column holds neither none nor one item per key.
     */
    answer<completion_, void> remove(keys_view_t keys, keys_view_t elements, keys_view_t end_keys = {},
                                     ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        std::size_t const count = keys.size();
        ustore_keys_view_t elements_view {}, end_keys_view {};
        status_t shaped = pages_column_(elements, count, elements_view);
        if (shaped.succeeded()) shaped = pages_column_(end_keys, count, end_keys_view);
        if (shaped.failed()) return refused_<completion_, void>(shaped);
        return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            ustore_pages_remove_t remove {};
            remove.database = receiver_.database;
            remove.executor = receiver_.executor;
            remove.outcome = outcome;
            remove.completion = completion_;
            remove.transaction = receiver_.transaction;
            remove.count = count;
            remove.collections = {&collection_, 0};
            remove.keys = {keys.data(), keys.stride()};
            remove.end_keys = end_keys_view;
            remove.elements = elements_view;
            remove.durability = durability;
            ustore_pages_remove(&remove);
        });
    }

    /**
     *  @brief Moves one element per task with its subtree under a parent after an anchor, and
     *      keeps the key it had.
     *  @param[in] keys Keys of the pages, borrowed until the call is final.
     *  @param[in] elements Elements to move, borrowed until the call is final.
     *  @param[in] parents The element every move lands under, or none for the root in every task,
     *      borrowed until the call is final.
     *  @param[in] anchors The sibling every move lands after, or none for the front in every task,
     *      borrowed until the call is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k A column holds neither none nor one item per key.
     */
    answer<completion_, void> move(keys_view_t keys, keys_view_t elements, keys_view_t parents = {},
                                   keys_view_t anchors = {},
                                   ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        std::size_t const count = keys.size();
        ustore_keys_view_t elements_view {}, parents_view {}, anchors_view {};
        status_t shaped = pages_column_(elements, count, elements_view);
        if (shaped.succeeded()) shaped = pages_column_(parents, count, parents_view);
        if (shaped.succeeded()) shaped = pages_column_(anchors, count, anchors_view);
        if (shaped.failed()) return refused_<completion_, void>(shaped);
        return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            ustore_pages_move_t move {};
            move.database = receiver_.database;
            move.executor = receiver_.executor;
            move.outcome = outcome;
            move.completion = completion_;
            move.transaction = receiver_.transaction;
            move.count = count;
            move.collections = {&collection_, 0};
            move.keys = {keys.data(), keys.stride()};
            move.elements = elements_view;
            move.parents = parents_view;
            move.anchors = anchors_view;
            move.durability = durability;
            ustore_pages_move(&move);
        });
    }

    /**
     *  @brief Replaces one run of text per task inside an element with new bytes.
     *  @param[in] keys Keys of the pages, borrowed until the call is final.
     *  @param[in] elements Elements whose text is spliced, borrowed until the call is final.
     *  @param[in] text_starts Byte into the text of every element where its splice starts, borrowed
     *      until the call is final.
     *  @param[in] text_lengths Bytes of text every splice replaces, where zero only inserts,
     *      borrowed until the call is final.
     *  @param[in] texts New bytes of every splice, where an empty run only removes, borrowed until
     *      the call is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k A column holds neither none nor one item per key.
     */
    template <strings_batch_concept texts_type_>
    answer<completion_, void> splice(keys_view_t keys, keys_view_t elements,
                                     strided_span<ustore_length_t const> text_starts,
                                     strided_span<ustore_length_t const> text_lengths, texts_type_ const& texts,
                                     ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        std::size_t const count = keys.size();
        ustore_keys_view_t elements_view {};
        ustore_lengths_view_t starts_view {}, lengths_view {};
        ustore_strings_view_t texts_view {};
        status_t shaped = pages_column_(elements, count, elements_view);
        if (shaped.succeeded()) shaped = pages_column_(text_starts, count, starts_view);
        if (shaped.succeeded()) shaped = pages_column_(text_lengths, count, lengths_view);
        if (shaped.succeeded()) shaped = pages_strings_(texts, count, texts_view);
        if (shaped.failed()) return refused_<completion_, void>(shaped);
        return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            ustore_pages_splice_t splice {};
            splice.database = receiver_.database;
            splice.executor = receiver_.executor;
            splice.outcome = outcome;
            splice.completion = completion_;
            splice.transaction = receiver_.transaction;
            splice.count = count;
            splice.collections = {&collection_, 0};
            splice.keys = {keys.data(), keys.stride()};
            splice.elements = elements_view;
            splice.text_starts = starts_view;
            splice.text_lengths = lengths_view;
            splice.values = texts_view;
            splice.durability = durability;
            ustore_pages_splice(&splice);
        });
    }

    /**
     *  @brief Sets one attribute per task of an element, adding the ones it lacks.
     *  @param[in] keys Keys of the pages, borrowed until the call is final.
     *  @param[in] elements Elements whose attributes are set, borrowed until the call is final.
     *  @param[in] fields Names of the attributes, borrowed until the call is final.
     *  @param[in] values Values of the attributes, borrowed until the call is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k A column holds neither none nor one item per key.
     */
    template <strings_batch_concept fields_type_, strings_batch_concept values_type_>
    answer<completion_, void> set_attributes(
        keys_view_t keys, keys_view_t elements, fields_type_ const& fields, values_type_ const& values,
        ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        std::size_t const count = keys.size();
        ustore_keys_view_t elements_view {};
        ustore_strings_view_t fields_view {}, values_view {};
        status_t shaped = pages_column_(elements, count, elements_view);
        if (shaped.succeeded()) shaped = pages_strings_(fields, count, fields_view);
        if (shaped.succeeded()) shaped = pages_strings_(values, count, values_view);
        if (shaped.failed()) return refused_<completion_, void>(shaped);
        return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            ustore_pages_set_attributes_t set {};
            set.database = receiver_.database;
            set.executor = receiver_.executor;
            set.outcome = outcome;
            set.completion = completion_;
            set.transaction = receiver_.transaction;
            set.count = count;
            set.collections = {&collection_, 0};
            set.keys = {keys.data(), keys.stride()};
            set.elements = elements_view;
            set.fields = fields_view;
            set.values = values_view;
            set.durability = durability;
            ustore_pages_set_attributes(&set);
        });
    }

    /**
     *  @brief Removes one attribute per task of an element.
     *  @param[in] keys Keys of the pages, borrowed until the call is final.
     *  @param[in] elements Elements whose attributes are removed, borrowed until the call is final.
     *  @param[in] fields Names of the attributes, borrowed until the call is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k A column holds neither none nor one item per key.
     */
    template <strings_batch_concept fields_type_>
    answer<completion_, void> remove_attributes(
        keys_view_t keys, keys_view_t elements, fields_type_ const& fields,
        ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        std::size_t const count = keys.size();
        ustore_keys_view_t elements_view {};
        ustore_strings_view_t fields_view {};
        status_t shaped = pages_column_(elements, count, elements_view);
        if (shaped.succeeded()) shaped = pages_strings_(fields, count, fields_view);
        if (shaped.failed()) return refused_<completion_, void>(shaped);
        return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            ustore_pages_remove_attributes_t remove {};
            remove.database = receiver_.database;
            remove.executor = receiver_.executor;
            remove.outcome = outcome;
            remove.completion = completion_;
            remove.transaction = receiver_.transaction;
            remove.count = count;
            remove.collections = {&collection_, 0};
            remove.keys = {keys.data(), keys.stride()};
            remove.elements = elements_view;
            remove.fields = fields_view;
            remove.durability = durability;
            ustore_pages_remove_attributes(&remove);
        });
    }

    /**
     *  @brief Renders the pages of @p keys, or the subtrees of @p elements inside them, into
     *      @p arena, where a missing page or element reads as a missing view.
     *  @param[in] keys Keys of the pages, borrowed until the call is final.
     *  @param[in] arena Memory the renderings land in, which the answer views, borrowed until the
     *      call is final.
     *  @param[in] elements Elements to render, or none for whole pages, borrowed until the
     *      call is final.
     *  @param[in] encoding Encoding the renderings come in.
     *  @param[in] watch Whether a read inside a transaction records the pages for conflicts.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k The @p elements are neither none nor one per key.
     */
    answer<completion_, values_view_t> read(keys_view_t keys, arena_t& arena, keys_view_t elements = {},
                                            ustore_page_encoding_t encoding = ustore_page_encoding_xml_k,
                                            ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        std::size_t const count = keys.size();
        ustore_keys_view_t elements_view {};
        if (status_t const shaped = pages_column_(elements, count, elements_view); shaped.failed())
            return refused_<completion_, values_view_t>(shaped);
        return called_<completion_, values_view_t>(
            [&](ustore_outcome_t* outcome, outputs<values_view_t>* written) noexcept {
                written->count = count;
                ustore_pages_read_t read {};
                read.database = receiver_.database;
                read.executor = receiver_.executor;
                read.outcome = outcome;
                read.completion = completion_;
                read.view = receiver_.view(watch);
                read.count = count;
                read.collections = {&collection_, 0};
                read.keys = {keys.data(), keys.stride()};
                read.elements = elements_view;
                read.presences = &written->presences;
                read.values = &written->values;
                read.arena = arena;
                read.outputs_encoding = encoding;
                ustore_pages_read(&read);
            });
    }

    /**
     *  @brief Draws up to @p wanted page keys at random into @p arena, rendering no pages, so a
     *      read follows the sample.
     *  @param[in] arena Memory the keys land in, which the answer views, borrowed until the
     *      call is final.
     *  @param[in] watch Whether a read inside a transaction records the keys for conflicts.
     */
    answer<completion_, std::span<ustore_key_t const>> sample(
        std::size_t wanted, arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return sample_keys_<completion_, ustore_pages_sample_t, &ustore_pages_sample>(receiver_, collection_, wanted,
                                                                                      arena, watch);
    }

    /**
     *  @brief Estimates the pages and the space from @p start_key, inclusive, to @p end_key,
     *      exclusive, into @p arena.
     *  @param[in] start_key First key of the range, borrowed until the call is final.
     *  @param[in] end_key Key the range stops before, borrowed until the call is final.
     *  @param[in] arena Memory the bounds land in, borrowed until the call is final.
     *  @param[in] watch Whether a read inside a transaction records the range for conflicts.
     */
    answer<completion_, size_estimates_t> measure(ustore_key_t const& start_key, ustore_key_t const& end_key,
                                                  arena_t& arena,
                                                  ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return measure_keys_<completion_, ustore_pages_measure_t, &ustore_pages_measure>(
            receiver_, collection_, start_key, end_key, arena, watch);
    }

    /** Removes every page, keeping the collection. */
    answer<completion_, void> clear() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_pairs_k);
    }

    /** Removes the collection with its name and all its pages. */
    answer<completion_, void> drop() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_collection_k);
    }

#pragma endregion Verbs

#pragma region Shorthand

    blobs_range_t members(ustore_key_t start_key = smallest_key_k, ustore_key_t end_key = largest_key_k) const noexcept
        requires blocks_k
    {
        return {receiver_, collection_, start_key, end_key, modality_k};
    }

    keys_range_t keys(ustore_key_t start_key = smallest_key_k, ustore_key_t end_key = largest_key_k) const noexcept
        requires blocks_k
    {
        return {members(start_key, end_key)};
    }

    /** Counts the pages by streaming through their keys. */
    expected<std::size_t> size() const noexcept
        requires blocks_k
    {
        return keys().size();
    }

#pragma endregion Shorthand
};

/** A collection of pages whose verbs block, reading and writing the head or a transaction. */
using pages_collection_t = pages_collection_on<ustore_completion_blocking_k, collection_access_t::read_write_k>;

/** A collection of pages whose verbs block and only read, as a snapshot hands out. */
using pages_read_collection_t = pages_collection_on<ustore_completion_blocking_k, collection_access_t::read_only_k>;

} // namespace unum::ustore
