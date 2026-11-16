/**
 *  @file cpp/ustore/docs_ref.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief The one body of every document verb, and the proxies reading, and modifying, a batch of
 *      documents or fields inside them through those bodies.
 */

#pragma once

#include <cstddef> // `std::ptrdiff_t`

#include <span>        // `std::span`
#include <type_traits> // `std::conditional_t`, `std::decay_t`, `std::is_rvalue_reference_v`
#include <utility>     // `std::forward`

#include "ustore/docs.h"
#include "ustore/docs_table.hpp" // `docs_table`, `table_header`, `table_header_view_t`
#include "ustore/ranges.hpp" // `values_view_t`, `lengths_view_t`, `bits_view_t`, `locations_places_`, `strided_strings`
#include "ustore/status.hpp" // `status_t`, `expected`, `answer`, `called_`
#include "ustore/types.hpp"  // `arena_t`, `one_off_answer_of`, `in_one_off_arena`, `receiver_t`, `collection_access_t`

namespace unum::ustore {

#pragma region Verbs

/**
 *  @brief Reads what @p answer_type_ asks of the documents at @p places, or of the fields @p fields
 *      name inside them, into @p arena: the values exported as @p type, their lengths, or only
 *      whether each is present.
 *  @param[in] places Collections and keys to read, borrowed until the call is final.
 *  @param[in] fields None for whole documents, or one field per place, borrowed until the
 *      call is final.
 *  @param[in] arena Memory the answer lands in, borrowed until the call is final, which
 *      the answer views.
 */
template <ustore_completion_t completion_, typename answer_type_>
answer<completion_, answer_type_> docs_read_(receiver_t const& receiver, place_batch_t const& places,
                                             ustore_strings_view_t fields, arena_t& arena,
                                             ustore_docs_field_type_t type, ustore_watch_t watch) noexcept {
    return called_<completion_, answer_type_>([&](ustore_outcome_t* outcome, outputs<answer_type_>* written) noexcept {
        written->count = places.count;
        ustore_docs_read_t read {};
        read.database = receiver.database;
        read.executor = receiver.executor;
        read.outcome = outcome;
        read.completion = completion_;
        read.view = receiver.view(watch);
        read.count = places.count;
        read.collections = places.collections();
        read.keys = places.keys();
        read.fields = fields;
        read.presences = &written->presences;
        read.lengths = lengths_output_(*written);
        read.values = values_output_(*written);
        read.arena = arena;
        read.outputs_type = type;
        ustore_docs_read(&read);
    });
}

/**
 *  @brief Writes @p values to the documents at @p places, or to the fields @p fields name inside
 *      them, as @p modification says.
 *  @param[in] values Values arriving as @p type, borrowed until the call is final.
 *  @param[in] durability Whether the call finishes only after the data reaches the disk.
 */
template <ustore_completion_t completion_>
answer<completion_, void> docs_write_(receiver_t const& receiver, place_batch_t const& places,
                                      ustore_strings_view_t fields, ustore_strings_view_t values,
                                      ustore_docs_field_type_t type, ustore_docs_modification_t modification,
                                      ustore_durability_t durability) noexcept {
    return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_docs_write_t write {};
        write.database = receiver.database;
        write.executor = receiver.executor;
        write.outcome = outcome;
        write.completion = completion_;
        write.transaction = receiver.transaction;
        write.count = places.count;
        write.collections = places.collections();
        write.keys = places.keys();
        write.fields = fields;
        write.values = values;
        write.type = type;
        write.modification = modification;
        write.durability = durability;
        ustore_docs_write(&write);
    });
}

/** Removes the documents at @p places whole, while a patch or a merge removes
 *  fields inside them. */
template <ustore_completion_t completion_>
answer<completion_, void> docs_remove_(receiver_t const& receiver, place_batch_t const& places,
                                       ustore_durability_t durability) noexcept {
    return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_docs_remove_t remove {};
        remove.database = receiver.database;
        remove.executor = receiver.executor;
        remove.outcome = outcome;
        remove.completion = completion_;
        remove.transaction = receiver.transaction;
        remove.count = places.count;
        remove.collections = places.collections();
        remove.keys = places.keys();
        remove.durability = durability;
        ustore_docs_remove(&remove);
    });
}

/** Lists the unique paths to the leaves of the documents at @p places into @p arena. */
template <ustore_completion_t completion_>
answer<completion_, sz::strings_view_t> docs_gist_(receiver_t const& receiver, place_batch_t const& places,
                                                   arena_t& arena, ustore_watch_t watch) noexcept {
    return called_<completion_, sz::strings_view_t>(
        [&](ustore_outcome_t* outcome, outputs<sz::strings_view_t>* written) noexcept {
            ustore_docs_gist_t gist {};
            gist.database = receiver.database;
            gist.executor = receiver.executor;
            gist.outcome = outcome;
            gist.completion = completion_;
            gist.view = receiver.view(watch);
            gist.count = places.count;
            gist.collections = places.collections();
            gist.keys = places.keys();
            gist.fields = &written->strings;
            gist.written = &written->written;
            gist.arena = arena;
            ustore_docs_gist(&gist);
        });
}

/** Gathers every column of @p header from the documents at @p places into @p arena, as a table
 *  whose index and header view the places and the header, both borrowed until the call is final and
 *  read by the table after. */
template <ustore_completion_t completion_, typename table_type_>
answer<completion_, table_type_> docs_gather_(receiver_t const& receiver, place_batch_t const& places,
                                              table_header_view_t const& header, arena_t& arena,
                                              ustore_watch_t watch) noexcept {
    return called_<completion_, table_type_>([&](ustore_outcome_t* outcome, outputs<table_type_>* written) noexcept {
        written->rows = places.count;
        written->collections = places.collections_begin;
        written->keys = places.keys_begin;
        written->header = header;
        ustore_docs_gather_t gather {};
        gather.database = receiver.database;
        gather.executor = receiver.executor;
        gather.outcome = outcome;
        gather.completion = completion_;
        gather.view = receiver.view(watch);
        gather.count = places.count;
        gather.collections = places.collections();
        gather.keys = places.keys();
        gather.fields_count = header.count;
        gather.fields = strings_view_of(std::span<std::string_view const> {header.fields_begin, header.count});
        gather.columns = &written->columns;
        gather.arena = arena;
        gather.outputs_types = header.types_begin;
        ustore_docs_gather(&gather);
    });
}

#pragma endregion Verbs

#pragma region Proxies

/**
 *  @brief Proxy reading documents, or fields inside them, at a batch of locations, through a
 *      snapshot, a transaction or the head, and modifying them too when @p access_ allows.
 *
 *  A read given an arena answers views into it, valid until the next call given that arena. The
 *  same read without one is shorthand: it makes a one-off arena and answers a result owning it, or
 *  a scalar copied out of it, which is slower than reusing an arena and keeps no state anywhere. A
 *  proxy assigns one value to one or many locations, or many values to as many locations. Copying
 *  is cheap, nothing throws, and every call blocks.
 *
 *  @tparam locations_type_ Addresses of the values: one key with an optional collection and field,
 *      an externally owned range of such addresses, or an array of them.
 */
template <typename locations_type_, collection_access_t access_ = collection_access_t::read_write_k>
class docs_ref {
  public:
    static_assert(!std::is_rvalue_reference_v<locations_type_>, "The locations can't be stored as an rvalue reference");

    using locations_store_t = location_store<locations_type_>;
    using locations_plain_t = typename locations_store_t::plain_t;
    static constexpr bool is_one_k = places_arg_extractor<locations_plain_t>::is_one_k;
    static constexpr bool writes_k = access_ == collection_access_t::read_write_k;

    using value_t = std::conditional_t<is_one_k, value_view_t, values_view_t>;
    using present_t = std::conditional_t<is_one_k, bool, bits_view_t>;
    using length_t = std::conditional_t<is_one_k, ustore_length_t, lengths_view_t>;

  private:
    receiver_t receiver_ {};
    locations_store_t locations_store_;
    ustore_docs_field_type_t type_ {ustore_docs_field_type_json_k};

    place_batch_t places_() const noexcept { return locations_places_(locations_store_.ref()); }

    /** Reads what @p batch_type_ asks into @p arena, in the format @p type, through the fields the
     *  locations name. */
    template <typename batch_type_>
    expected<batch_type_> read_(arena_t& arena, ustore_docs_field_type_t type, ustore_watch_t watch) noexcept {
        place_batch_t const places = places_();
        strided_strings<std::string_view> const fields {places.fields_begin};
        return docs_read_<ustore_completion_blocking_k, batch_type_>(receiver_, places, fields.view(), arena, type,
                                                                     watch);
    }

    template <typename values_argument_type_>
    status_t write_(values_argument_type_&& values_argument, ustore_docs_modification_t modification,
                    ustore_docs_field_type_t type, ustore_durability_t durability) noexcept {
        // An array decays into one pointer object, whose address an accessor can stride over
        using values_t = std::decay_t<values_argument_type_>;
        values_t const& values = values_argument;
        values_arg_extractor<values_t> extractor;
        place_batch_t const places = places_();
        strided_strings<std::string_view> const fields {places.fields_begin};
        return docs_write_<ustore_completion_blocking_k>(receiver_, places, fields.view(), extractor.view(values), type,
                                                         modification, durability);
    }

  public:
    docs_ref(receiver_t receiver, locations_type_&& locations,
             ustore_docs_field_type_t type = ustore_docs_field_type_json_k) noexcept
        : receiver_(receiver), locations_store_(std::forward<locations_type_>(locations)), type_(type) {}

    docs_ref& as(ustore_docs_field_type_t type) noexcept {
        type_ = type;
        return *this;
    }

    /** Reads the documents or fields into @p arena, in the format of the proxy. */
    expected<value_t> value(arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return first_or_all_<value_t>(read_<values_view_t>(arena, type_, watch));
    }

    /** Reads the documents or fields into a one-off arena, which the answer owns, in the format of
     *  the proxy. */
    expected<one_off_answer_of<value_t>> value(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return value(type_, watch);
    }

    /** Reads the documents or fields into a one-off arena, which the answer owns, in the format @p
     *  type. */
    expected<one_off_answer_of<value_t>> value(ustore_docs_field_type_t type,
                                               ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return in_one_off_arena(receiver_.executor, [&](arena_t& arena) noexcept {
            return first_or_all_<value_t>(read_<values_view_t>(arena, type, watch));
        });
    }

    /** Reads the lengths of the documents or fields into @p arena. */
    expected<length_t> length(arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return first_or_all_<length_t>(read_<lengths_view_t>(arena, type_, watch));
    }

    /** Reads the lengths of the documents or fields into a one-off arena. */
    expected<one_off_answer_of<length_t>> length(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return in_one_off_arena(receiver_.executor, [&](arena_t& arena) noexcept { return length(arena, watch); });
    }

    /** Checks which locations hold a value, which may be empty, into @p arena. */
    expected<present_t> present(arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return first_or_all_<present_t>(read_<bits_view_t>(arena, type_, watch));
    }

    /** Checks which locations hold a value, which may be empty, into a one-off arena. */
    expected<one_off_answer_of<present_t>> present(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return in_one_off_arena(receiver_.executor, [&](arena_t& arena) noexcept { return present(arena, watch); });
    }

    /** Lists the unique paths to the leaves of the documents into @p arena. */
    expected<sz::strings_view_t> gist(arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return docs_gist_<ustore_completion_blocking_k>(receiver_, places_(), arena, watch);
    }

    /** Gathers every column of @p header from every document into @p arena, as a table typed at
     *  runtime. */
    expected<docs_table_t> gather(table_header_view_t const& header, arena_t& arena,
                                  ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return docs_gather_<ustore_completion_blocking_k, docs_table_t>(receiver_, places_(), header, arena, watch);
    }

    /** Gathers every column of @p header from every document into @p arena, as a table typed at
     *  compile time. */
    template <typename... column_types_>
    expected<docs_table<column_types_...>> gather(table_header<column_types_...> const& header, arena_t& arena,
                                                  ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return docs_gather_<ustore_completion_blocking_k, docs_table<column_types_...>>(receiver_, places_(),
                                                                                        header.view(), arena, watch);
    }

    /** Upserts @p values pair-wise into the locations, in the format of the proxy. */
    template <typename values_argument_type_>
    status_t assign(values_argument_type_&& values,
                    ustore_durability_t durability = ustore_durability_buffered_k) noexcept
        requires writes_k
    {
        return write_(std::forward<values_argument_type_>(values), ustore_docs_modify_upsert_k, type_, durability);
    }

    /** Upserts @p values pair-wise into the locations, in the format @p type. */
    template <typename values_argument_type_>
    status_t assign(values_argument_type_&& values, ustore_docs_field_type_t type,
                    ustore_durability_t durability = ustore_durability_buffered_k) noexcept
        requires writes_k
    {
        return write_(std::forward<values_argument_type_>(values), ustore_docs_modify_upsert_k, type, durability);
    }

    /**
     *  @brief Removes the documents whole, refusing locations that name a field, which a patch or
     *      a merge removes.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k A location names a field.
     */
    status_t remove(ustore_durability_t durability = ustore_durability_buffered_k) noexcept
        requires writes_k
    {
        // The removal takes no fields, so only this side sees a location narrowed to one
        place_batch_t const places = places_();
        for (std::size_t index = 0; places.fields_begin && index != places.count; ++index)
            if (!places.fields_begin[static_cast<std::ptrdiff_t>(index)].empty())
                return status_t {status_code_t::invalid_argument_k, "A field is removed by a patch or a merge"};
        return docs_remove_<ustore_completion_blocking_k>(receiver_, places, durability);
    }

    /** Keeps the keys, but empties the associated values. */
    status_t clear_values(ustore_durability_t durability = ustore_durability_buffered_k) noexcept
        requires writes_k
    {
        return assign(value_view_t::make_empty(), durability);
    }

    /** Patches the documents with RFC 6902 JSON Patches. */
    template <typename values_argument_type_>
    status_t patch(values_argument_type_&& values,
                   ustore_durability_t durability = ustore_durability_buffered_k) noexcept
        requires writes_k
    {
        return write_(std::forward<values_argument_type_>(values), ustore_docs_modify_patch_k, type_, durability);
    }

    /** Patches the documents with RFC 7386 JSON Merge Patches. */
    template <typename values_argument_type_>
    status_t merge(values_argument_type_&& values,
                   ustore_durability_t durability = ustore_durability_buffered_k) noexcept
        requires writes_k
    {
        return write_(std::forward<values_argument_type_>(values), ustore_docs_modify_merge_k, type_, durability);
    }

    /** Inserts @p values, failing where a document already exists. */
    template <typename values_argument_type_>
    status_t insert(values_argument_type_&& values,
                    ustore_durability_t durability = ustore_durability_buffered_k) noexcept
        requires writes_k
    {
        return write_(std::forward<values_argument_type_>(values), ustore_docs_modify_insert_k, type_, durability);
    }

    /** Inserts @p values, replacing existing documents. */
    template <typename values_argument_type_>
    status_t upsert(values_argument_type_&& values,
                    ustore_durability_t durability = ustore_durability_buffered_k) noexcept
        requires writes_k
    {
        return write_(std::forward<values_argument_type_>(values), ustore_docs_modify_upsert_k, type_, durability);
    }

    /** Replaces existing documents or fields with @p values. */
    template <typename values_argument_type_>
    status_t update(values_argument_type_&& values,
                    ustore_durability_t durability = ustore_durability_buffered_k) noexcept
        requires writes_k
    {
        return write_(std::forward<values_argument_type_>(values), ustore_docs_modify_update_k, type_, durability);
    }

    locations_plain_t& locations() noexcept { return locations_store_.ref(); }
    locations_plain_t const& locations() const noexcept { return locations_store_.ref(); }
};

/** A proxy that only reads, as snapshots hand out. */
template <typename locations_type_>
using docs_read_ref = docs_ref<locations_type_, collection_access_t::read_only_k>;

static_assert(docs_read_ref<ustore_key_t>::is_one_k);
static_assert(std::is_same_v<docs_read_ref<ustore_key_t>::value_t, value_view_t>);
static_assert(!docs_read_ref<place_batch_t>::is_one_k);

#pragma endregion Proxies

} // namespace unum::ustore
