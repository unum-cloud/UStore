/**
 *  @file cpp/ustore/docs_collection.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief Handles to a collection of JSON documents, whose verbs block or answer deferred calls as
 *      their receiver chose.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <initializer_list> // `std::initializer_list`
#include <iterator>         // `std::size`
#include <span>             // `std::span`
#include <utility>          // `std::forward`

#include "ustore/blobs_range.hpp" // `blobs_range_t`, `keys_range_t`, `size_range_t`, `sample_keys_`, `measure_keys_`
#include "ustore/docs.h"
#include "ustore/docs_ref.hpp" // `docs_ref`, `docs_read_`, `docs_write_`, `docs_remove_`, `docs_gist_`, `docs_gather_`
#include "ustore/docs_table.hpp" // `docs_table`, `table_header`, `table_header_view_t`
#include "ustore/ranges.hpp"     // `places_in`, `keys_view_t`, `strings_batch_concept`, `repeated_view_of`
#include "ustore/status.hpp"     // `status_t`, `expected`, `answer`, `refused_`, `defers`
#include "ustore/types.hpp"      // `arena_t`, `receiver_t`, `collection_access_t`, `location_in`, `collection_drop_`

namespace unum::ustore {

/** The fields a call over @p count keys passes as @p fields: none for whole documents, one for
 *  every key, which an accessor whose handle is @p fields repeats, or one per key. */
template <strings_batch_concept fields_type_>
status_t fields_view_(fields_type_ const& fields, std::size_t count, ustore_strings_view_t& view) noexcept {
    std::size_t const given = std::size(fields);
    if (given == 0) view = {};
    else if (given == count) view = strings_view_of(fields);
    else if (given == 1) view = repeated_view_of(fields);
    else return status_t {status_code_t::invalid_argument_k, "Every key needs its own field, or one field serves all"};
    return {};
}

/**
 *  @brief A handle to one collection of JSON documents, cheap to construct and to copy, which
 *      addresses the head or the view of a transaction or snapshot, and whose verbs answer as
 *      @p completion_ says.
 *
 *  A blocking handle answers an @c outcome, while a deferred one answers a @c deferred call,
 *  viewing the handle, its inputs and its arena until the call is final. A handle with
 *  @c collection_access_t::read_only_k only reads, as a snapshot hands out. The type of a handle
 *  shapes only how values cross it, since documents are stored as JSON whatever they arrive as.
 */
template <ustore_completion_t completion_, collection_access_t access_>
class docs_collection_on {
    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {};
    ustore_docs_field_type_t type_ {ustore_docs_field_type_json_k};

    static constexpr bool blocks_k = !defers<completion_>;
    static constexpr bool writes_k = access_ == collection_access_t::read_write_k;

    /** A proxy over one location inside this collection, read and written as @p type. */
    template <typename key_argument_type_>
        requires(!batch_concept<key_argument_type_>)
    docs_ref<collection_key_field_t, access_> address_(key_argument_type_ const& key,
                                                       ustore_docs_field_type_t type) const noexcept {
        return {receiver_, location_in(collection_, key), type};
    }

    /** A proxy over a batch of locations inside this collection, read and written as @p type. */
    template <batch_concept keys_argument_type_>
    auto address_(keys_argument_type_&& keys, ustore_docs_field_type_t type) const noexcept {
        using locations_t = locations_in_collection<keys_argument_type_>;
        return docs_ref<locations_t, access_> {
            receiver_, locations_t {std::forward<keys_argument_type_>(keys), collection_}, type};
    }

  public:
    /** The handle a snapshot hands out for this kind of collection, which only reads. */
    using read_only_t = docs_collection_on<completion_, collection_access_t::read_only_k>;

    /** The same kind of handle, completing its verbs as @p other_completion_ says. */
    template <ustore_completion_t other_completion_>
    using completed_as = docs_collection_on<other_completion_, access_>;

    /** What the collections this kind of handle addresses hold, which a creation records and a
     *  lookup checks. */
    static constexpr ustore_modality_t modality_k = ustore_modality_docs_k;

    docs_collection_on() noexcept = default;
    docs_collection_on(receiver_t receiver, ustore_collection_id_t collection,
                       ustore_docs_field_type_t type = ustore_docs_field_type_json_k) noexcept
        : receiver_(receiver), collection_(collection), type_(type) {}

    operator ustore_collection_id_t() const noexcept { return collection_; }
    ustore_collection_id_t* member_ptr() noexcept { return &collection_; }
    receiver_t receiver() const noexcept { return receiver_; }

    /** The same collection, whose verbs block on @p executor, or on the calling
     *  thread without one. */
    completed_as<ustore_completion_blocking_k> on(ustore_executor_t executor) const noexcept {
        return {receiver_.on(executor), collection_, type_};
    }

    /** The same collection, whose verbs submit to @p executor and answer deferred calls. */
    completed_as<ustore_completion_deferred_k> deferred(ustore_executor_t executor) const noexcept {
        return {receiver_.on(executor), collection_, type_};
    }

#pragma region Verbs

    /**
     *  @brief Reads the documents of @p keys, or the fields @p fields name inside them,
     *      into @p arena.
     *  @param[in] keys Keys to read, owned by the caller until the call is final.
     *  @param[in] arena Memory the values land in, owned by the caller until the call is final,
     *      which the answer views.
     *  @param[in] fields None for whole documents, one field for every key, or one per key,
     *      owned by the caller until final.
     *  @param[in] type Format the values are exported in.
     *  @param[in] watch Whether a read inside a transaction records the keys for conflicts.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k The @p fields are neither none, one, nor one per key.
     */
    template <strings_batch_concept fields_type_ = values_tape_t>
    answer<completion_, values_view_t> read(keys_view_t keys, arena_t& arena, fields_type_ const& fields = {},
                                            ustore_docs_field_type_t type = ustore_docs_field_type_json_k,
                                            ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        ustore_strings_view_t fields_view {};
        if (status_t const pointed = fields_view_(fields, keys.size(), fields_view); pointed.failed())
            return refused_<completion_, values_view_t>(pointed);
        return docs_read_<completion_, values_view_t>(receiver_, places_in(collection_, keys), fields_view, arena, type,
                                                      watch);
    }

    /**
     *  @brief Writes @p values to the documents of @p keys pairwise, or to the fields @p fields
     *      name inside them.
     *  @param[in] keys Keys to write, owned by the caller until the call is final.
     *  @param[in] values One value per key, owned by the caller until the call is final.
     *  @param[in] fields None for whole documents, one field for every key, or one per key,
     *      owned by the caller until final.
     *  @param[in] modification Whether the values upsert, update or insert, or patch or merge
     *      what they find.
     *  @param[in] type Format the values arrive in.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k The @p keys and @p values differ in count, or the
     *    @p fields are neither none, one, nor one per key.
     */
    template <strings_batch_concept values_type_, strings_batch_concept fields_type_ = values_tape_t>
    answer<completion_, void> write(keys_view_t keys, values_type_ const& values, fields_type_ const& fields = {},
                                    ustore_docs_modification_t modification = ustore_docs_modify_upsert_k,
                                    ustore_docs_field_type_t type = ustore_docs_field_type_json_k,
                                    ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        // The call has one count, so only this side sees the keys and the values disagree
        if (keys.size() != std::size(values))
            return refused_<completion_, void>({status_code_t::invalid_argument_k, "Every key needs its own value"});
        ustore_strings_view_t fields_view {};
        if (status_t const pointed = fields_view_(fields, keys.size(), fields_view); pointed.failed())
            return refused_<completion_, void>(pointed);
        return docs_write_<completion_>(receiver_, places_in(collection_, keys), fields_view, strings_view_of(values),
                                        type, modification, durability);
    }

    /**
     *  @brief Removes the documents of @p keys whole, while a patch or a merge removes
     *      fields inside them.
     *  @param[in] keys Keys to remove, owned by the caller until the call is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     */
    answer<completion_, void> remove(keys_view_t keys,
                                     ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        return docs_remove_<completion_>(receiver_, places_in(collection_, keys), durability);
    }

    /**
     *  @brief Lists the unique paths to the leaves of the documents of @p keys into @p arena.
     *  @param[in] keys Keys of the documents, owned by the caller until the call is final.
     *  @param[in] arena Memory the paths land in, owned by the caller until the call is final,
     *      which the answer views.
     *  @param[in] watch Whether a read inside a transaction records the keys for conflicts.
     */
    answer<completion_, sz::strings_view_t> gist(keys_view_t keys, arena_t& arena,
                                                 ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return docs_gist_<completion_>(receiver_, places_in(collection_, keys), arena, watch);
    }

    /**
     *  @brief Gathers the columns @p header names from the documents of @p keys into @p arena, as
     *      a table typed at runtime, whose index views @p keys and this handle and whose header
     *      views @p header.
     *  @param[in] keys Keys of the documents, one per row, owned by the caller until the call is
     *      final and read by the table.
     *  @param[in] header Field and type of every column, owned by the caller until the call is
     *      final and read by the table.
     *  @param[in] arena Memory the columns land in, owned by the caller until the call is final,
     *      which the table views.
     *  @param[in] watch Whether a read inside a transaction records the keys for conflicts.
     */
    answer<completion_, docs_table_t> gather(keys_view_t keys, table_header_view_t const& header, arena_t& arena,
                                             ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return docs_gather_<completion_, docs_table_t>(receiver_, places_in(collection_, keys), header, arena, watch);
    }

    /** Gathers the columns @p header names from the documents of @p keys into @p arena, as a table
     *  typed at compile time, viewing what the runtime-typed form views. */
    template <typename... column_types_>
    answer<completion_, docs_table<column_types_...>> gather(
        keys_view_t keys, table_header<column_types_...> const& header, arena_t& arena,
        ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return docs_gather_<completion_, docs_table<column_types_...>>(receiver_, places_in(collection_, keys),
                                                                       header.view(), arena, watch);
    }

    /**
     *  @brief Draws up to @p wanted document keys at random into @p arena, exporting no documents,
     *      so a read or a gather follows the sample.
     *  @param[in] arena Memory the keys land in, which the answer views, viewed until the
     *      call is final.
     *  @param[in] watch Whether a read inside a transaction records the keys for conflicts.
     */
    answer<completion_, std::span<ustore_key_t const>> sample(
        std::size_t wanted, arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return sample_keys_<completion_, ustore_docs_sample_t, &ustore_docs_sample>(receiver_, collection_, wanted,
                                                                                    arena, watch);
    }

    /**
     *  @brief Estimates the documents and the space from @p start_key, inclusive, to @p end_key,
     *      exclusive, into @p arena.
     *  @param[in] start_key First key of the range, owned by the caller until the call is final.
     *  @param[in] end_key Key the range stops before, owned by the caller until the call is final.
     *  @param[in] arena Memory the bounds land in, owned by the caller until the call is final.
     *  @param[in] watch Whether a read inside a transaction records the range for conflicts.
     */
    answer<completion_, size_estimates_t> measure(ustore_key_t const& start_key, ustore_key_t const& end_key,
                                                  arena_t& arena,
                                                  ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return measure_keys_<completion_, ustore_docs_measure_t, &ustore_docs_measure>(
            receiver_, collection_, start_key, end_key, arena, watch);
    }

    /** Empties every document, keeping the keys. */
    answer<completion_, void> clear_values() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_values_k);
    }

    /** Removes every document, keeping the collection. */
    answer<completion_, void> clear() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_pairs_k);
    }

    /** Removes the collection with all its documents. */
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

    /** Counts the documents by streaming through their keys, both bounds being the count. */
    expected<size_range_t> size_range() const noexcept
        requires blocks_k
    {
        auto [count, count_status] = keys().size();
        if (count_status.failed()) return count_status;
        return size_range_t {count, count};
    }

    /** Estimates the number of documents as the middle of @c size_range. */
    expected<std::size_t> size() const noexcept
        requires blocks_k
    {
        auto [range, size_range_status] = size_range();
        if (size_range_status.failed()) return size_range_status;
        return (range.min + range.max) / 2;
    }

    docs_ref<place_batch_t, access_> operator[](std::initializer_list<ustore_key_t> keys) const noexcept
        requires blocks_k
    {
        return at(keys);
    }

    docs_ref<place_batch_t, access_> at(std::initializer_list<ustore_key_t> keys) const noexcept
        requires blocks_k
    {
        return at(keys_view_t {strided(keys)});
    }

    docs_ref<place_batch_t, access_> operator[](keys_view_t keys) const noexcept
        requires blocks_k
    {
        return at(keys);
    }

    docs_ref<place_batch_t, access_> at(keys_view_t keys) const noexcept
        requires blocks_k
    {
        return at(keys, type_);
    }

    docs_ref<place_batch_t, access_> at(keys_view_t keys, ustore_docs_field_type_t type) const noexcept
        requires blocks_k
    {
        return {receiver_, places_in(collection_, keys), type};
    }

    template <typename keys_argument_type_>
    auto operator[](keys_argument_type_&& keys) const noexcept
        requires blocks_k
    {
        return at(std::forward<keys_argument_type_>(keys));
    }

    template <typename keys_argument_type_>
    auto at(keys_argument_type_&& keys) const noexcept
        requires blocks_k
    {
        return at(std::forward<keys_argument_type_>(keys), type_);
    }

    /** Addresses one location or a batch of them inside this collection, read and written as @p
     *  type. */
    template <typename keys_argument_type_>
    auto at(keys_argument_type_&& keys, ustore_docs_field_type_t type) const noexcept
        requires blocks_k
    {
        return address_(std::forward<keys_argument_type_>(keys), type);
    }

#pragma endregion Shorthand
};

/** A collection of JSON documents whose verbs block, reading and writing the head
 *  or a transaction. */
using docs_collection_t = docs_collection_on<ustore_completion_blocking_k, collection_access_t::read_write_k>;

/** A collection of JSON documents whose verbs block and only read, as a snapshot hands out. */
using docs_read_collection_t = docs_collection_on<ustore_completion_blocking_k, collection_access_t::read_only_k>;

} // namespace unum::ustore
