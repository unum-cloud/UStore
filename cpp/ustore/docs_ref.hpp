/**
 *  @file cpp/ustore/docs_ref.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief Proxies reading, and modifying, a batch of documents or fields inside them.
 */

#pragma once

#include <cstddef> // `std::ptrdiff_t`

#include <type_traits> // `std::conditional_t`, `std::decay_t`, `std::is_same_v`
#include <utility>     // `std::forward`

#include "ustore/docs.h"
#include "ustore/docs_table.hpp" // `docs_table`, `table_header`, `table_header_view_t`
#include "ustore/ranges.hpp"     // `values_view_t`, `lengths_view_t`, `bits_view_t`, `strings_of`, `location_store`
#include "ustore/status.hpp"     // `status_t`, `expected`
#include "ustore/types.hpp"      // `arena_t`, `one_off_answer_of`, `in_one_off_arena`, `receiver_t`

namespace unum::ustore {

/**
 *  @brief Proxy reading documents, or fields inside them, at a batch of locations, through a snapshot, a transaction
 *      or the head, which never writes them.
 *
 *  A read given an arena answers views into it, valid until the next call given that arena. The same read without one
 *  is shorthand: it makes a one-off arena and answers a result owning it, or a scalar copied out of it, which is
 *  slower than reusing an arena and keeps no state anywhere. Copying is cheap and nothing throws.
 *
 *  @tparam locations_type_ Addresses of the values: one key with an optional collection and field,
 *      an externally owned range of such addresses, or an array of them.
 */
template <typename locations_type_>
class docs_read_ref {
  public:
    static_assert(!std::is_rvalue_reference_v<locations_type_>, "The locations can't be stored as an rvalue reference");

    using locations_store_t = location_store<locations_type_>;
    using locations_plain_t = typename locations_store_t::plain_t;
    using keys_extractor_t = places_arg_extractor<locations_plain_t>;
    static constexpr bool is_one_k = keys_extractor_t::is_one_k;

    using value_t = std::conditional_t<is_one_k, value_view_t, values_view_t>;
    using present_t = std::conditional_t<is_one_k, bool, bits_view_t>;
    using length_t = std::conditional_t<is_one_k, ustore_length_t, lengths_view_t>;

  protected:
    receiver_t receiver_ {};
    locations_store_t locations_store_;
    ustore_docs_field_type_t type_ {ustore_docs_field_type_json_k};

  public:
    docs_read_ref(receiver_t receiver, locations_type_&& locations,
                  ustore_docs_field_type_t type = ustore_docs_field_type_json_k) noexcept
        : receiver_(receiver), locations_store_(std::forward<locations_type_>(locations)), type_(type) {}

    docs_read_ref& as(ustore_docs_field_type_t type) noexcept {
        type_ = type;
        return *this;
    }

    /** Reads the documents or fields into @p arena, in the format of the proxy. */
    expected<value_t> value(arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return read_<value_t>(arena, type_, watch);
    }

    /** Reads the documents or fields into a one-off arena, which the answer owns, in the format of the proxy. */
    expected<one_off_answer_of<value_t>> value(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return value(type_, watch);
    }

    /** Reads the documents or fields into a one-off arena, which the answer owns, in the format @p type. */
    expected<one_off_answer_of<value_t>> value(ustore_docs_field_type_t type,
                                               ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return in_one_off_arena(receiver_.executor,
                                [&](arena_t& arena) noexcept { return read_<value_t>(arena, type, watch); });
    }

    /** Reads the lengths of the documents or fields into @p arena. */
    expected<length_t> length(arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return read_<length_t>(arena, type_, watch);
    }

    /** Reads the lengths of the documents or fields into a one-off arena. */
    expected<one_off_answer_of<length_t>> length(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return in_one_off_arena(receiver_.executor, [&](arena_t& arena) noexcept { return length(arena, watch); });
    }

    /** Checks which locations hold a value, which may be empty, into @p arena. */
    expected<present_t> present(arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return read_<present_t>(arena, type_, watch);
    }

    /** Checks which locations hold a value, which may be empty, into a one-off arena. */
    expected<one_off_answer_of<present_t>> present(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return in_one_off_arena(receiver_.executor, [&](arena_t& arena) noexcept { return present(arena, watch); });
    }

    locations_plain_t& locations() noexcept { return locations_store_.ref(); }
    locations_plain_t const& locations() const noexcept { return locations_store_.ref(); }

    /** Lists the unique paths to the leaves of the documents into @p arena. */
    expected<sz::strings_view_t> gist(arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        ustore_size_t found_count = 0;
        ustore_strings_view_t found_fields {};

        decltype(auto) places = locations_store_.ref();
        strided_iterator<ustore_collection_id_t const> const collections = keys_extractor_t {}.collections(places);
        strided_iterator<ustore_key_t const> const keys = keys_extractor_t {}.keys(places);

        ustore_outcome_t outcome {};
        ustore_docs_gist_t docs_gist {};
        docs_gist.database = receiver_.database;
        docs_gist.executor = receiver_.executor;
        docs_gist.outcome = &outcome;
        docs_gist.view = receiver_.view(watch);
        docs_gist.count = keys_extractor_t {}.count(places);
        docs_gist.collections = {collections.get(), collections.stride()};
        docs_gist.keys = {keys.get(), keys.stride()};
        docs_gist.fields = &found_fields;
        docs_gist.written = &found_count;
        docs_gist.arena = arena;

        ustore_docs_gist(&docs_gist);
        if (status_t const status {outcome}; status.failed()) return status;
        return strings_of(found_fields, found_count);
    }

    /** Gathers every column of @p header from every document into @p arena, as a table typed at runtime. */
    expected<docs_table_t> gather(table_header_view_t const& header, arena_t& arena,
                                  ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return gather_<docs_table_t>(header, arena, watch);
    }

    /** Gathers every column of @p header from every document into @p arena, as a table typed at compile time. */
    template <typename... column_types_>
    expected<docs_table<column_types_...>> gather(table_header<column_types_...> const& header, arena_t& arena,
                                                  ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return gather_<docs_table<column_types_...>>(header, arena, watch);
    }

  protected:
    template <typename answer_type_>
    expected<answer_type_> read_(arena_t& arena, ustore_docs_field_type_t type, ustore_watch_t watch) noexcept {
        decltype(auto) places = locations_store_.ref();
        ustore_size_t const count = keys_extractor_t {}.count(places);
        strided_iterator<ustore_collection_id_t const> const collections = keys_extractor_t {}.collections(places);
        strided_iterator<ustore_key_t const> const keys = keys_extractor_t {}.keys(places);
        strided_strings<std::string_view> const fields {keys_extractor_t {}.fields(places)};

        ustore_octet_t const* presences = nullptr;
        ustore_length_t const* lengths = nullptr;
        ustore_strings_view_t values {};
        ustore_outcome_t outcome {};
        ustore_docs_read_t docs_read {};
        docs_read.database = receiver_.database;
        docs_read.executor = receiver_.executor;
        docs_read.outcome = &outcome;
        docs_read.view = receiver_.view(watch);
        docs_read.count = count;
        docs_read.collections = {collections.get(), collections.stride()};
        docs_read.keys = {keys.get(), keys.stride()};
        docs_read.fields = fields.view();
        docs_read.presences = &presences;
        docs_read.lengths = std::is_same_v<answer_type_, length_t> ? &lengths : nullptr;
        docs_read.values = std::is_same_v<answer_type_, value_t> ? &values : nullptr;
        docs_read.arena = arena;
        docs_read.outputs_type = type;

        ustore_docs_read(&docs_read);
        if (status_t const status {outcome}; status.failed()) return status;

        if constexpr (std::is_same_v<answer_type_, length_t>) {
            lengths_view_t const found {count, presences, lengths};
            if constexpr (is_one_k) return found[0];
            else return found;
        }
        else if constexpr (std::is_same_v<answer_type_, present_t>) {
            bits_view_t const present {presences};
            if constexpr (is_one_k) return present[0];
            else return present;
        }
        else {
            values_view_t const found {count, presences, values};
            if constexpr (is_one_k) return found[0];
            else return found;
        }
    }

    template <typename table_type_, typename header_type_>
    expected<table_type_> gather_(header_type_ const& header, arena_t& arena, ustore_watch_t watch) noexcept {
        decltype(auto) places = locations_store_.ref();
        ustore_size_t const count = keys_extractor_t {}.count(places);
        strided_iterator<ustore_collection_id_t const> const collections = keys_extractor_t {}.collections(places);
        strided_iterator<ustore_key_t const> const keys = keys_extractor_t {}.keys(places);
        strided_span<std::string_view const> const fields = header.fields();
        strided_span<ustore_docs_field_type_t const> const types = header.types();
        strided_strings<std::string_view> const field_names {fields.begin()};

        // The C ABI reads one type per field back to back, so a header striding over its columns lends a copy
        st::vector<ustore_docs_field_type_t> packed;
        ustore_docs_field_type_t const* packed_types = types.data();
        if (types.size() > 1 && types.stride() != sizeof(ustore_docs_field_type_t)) {
            if (st::failed(packed.resize(types.size())))
                return status_t {status_code_t::out_of_memory_k, "Failed to pack the types of a gather"};
            for (std::size_t index = 0; index != types.size(); ++index) packed[index] = types[index];
            packed_types = packed.data();
        }

        ustore_docs_column_view_t const* columns = nullptr;
        ustore_outcome_t outcome {};
        ustore_docs_gather_t docs_gather {};
        docs_gather.database = receiver_.database;
        docs_gather.executor = receiver_.executor;
        docs_gather.outcome = &outcome;
        docs_gather.view = receiver_.view(watch);
        docs_gather.count = count;
        docs_gather.collections = {collections.get(), collections.stride()};
        docs_gather.keys = {keys.get(), keys.stride()};
        docs_gather.fields_count = fields.size();
        docs_gather.fields = field_names.view();
        docs_gather.columns = &columns;
        docs_gather.arena = arena;
        docs_gather.outputs_types = packed_types;

        ustore_docs_gather(&docs_gather);
        if (status_t const status {outcome}; status.failed()) return status;
        return table_type_ {count, fields.size(), collections, keys, fields.begin(), types.begin(), columns};
    }
};

/**
 *  @brief Proxy reading and writing documents, or fields inside them, at a batch of locations, at the head or through
 *      a transaction.
 *
 *  A proxy assigns one value to one or many locations, or many values to as many locations.
 *
 *  @tparam locations_type_ Addresses of the values: one key with an optional collection and field,
 *      an externally owned range of such addresses, or an array of them.
 */
template <typename locations_type_>
class docs_ref : public docs_read_ref<locations_type_> {
    using base_t = docs_read_ref<locations_type_>;
    using keys_extractor_t = typename base_t::keys_extractor_t;

  public:
    using base_t::base_t;

    docs_ref& as(ustore_docs_field_type_t type) noexcept {
        base_t::as(type);
        return *this;
    }

    /** Upserts @p values pair-wise into the locations, in the format of the proxy. */
    template <typename values_argument_type_>
    status_t assign(values_argument_type_&& values,
                    ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return any_write(std::forward<values_argument_type_>(values), ustore_docs_modify_upsert_k, this->type_,
                         durability);
    }

    /** Upserts @p values pair-wise into the locations, in the format @p type. */
    template <typename values_argument_type_>
    status_t assign(values_argument_type_&& values, ustore_docs_field_type_t type,
                    ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return any_write(std::forward<values_argument_type_>(values), ustore_docs_modify_upsert_k, type, durability);
    }

    /**
     *  Removes the documents whole, refusing locations that name a field, which a patch or a merge removes.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k A location names a field.
     */
    status_t remove(ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        decltype(auto) places = this->locations_store_.ref();
        ustore_size_t const count = keys_extractor_t {}.count(places);
        strided_iterator<ustore_collection_id_t const> const collections = keys_extractor_t {}.collections(places);
        strided_iterator<ustore_key_t const> const keys = keys_extractor_t {}.keys(places);
        strided_iterator<std::string_view const> const fields = keys_extractor_t {}.fields(places);
        for (std::size_t index = 0; fields && index != count; ++index)
            if (!fields[static_cast<std::ptrdiff_t>(index)].empty())
                return status_t {status_code_t::invalid_argument_k, "A field is removed by a patch or a merge"};

        ustore_outcome_t outcome {};
        ustore_docs_remove_t remove {};
        remove.database = this->receiver_.database;
        remove.executor = this->receiver_.executor;
        remove.outcome = &outcome;
        remove.transaction = this->receiver_.transaction;
        remove.count = count;
        remove.collections = {collections.get(), collections.stride()};
        remove.keys = {keys.get(), keys.stride()};
        remove.durability = durability;
        ustore_docs_remove(&remove);
        return status_t {outcome};
    }

    /** Keeps the keys, but empties the associated values. */
    status_t clear_values(ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return assign(value_view_t::make_empty(), durability);
    }

    /** Patches the documents with RFC 6902 JSON Patches. */
    template <typename values_argument_type_>
    status_t patch(values_argument_type_&& values,
                   ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return any_write(std::forward<values_argument_type_>(values), ustore_docs_modify_patch_k, this->type_,
                         durability);
    }

    /** Patches the documents with RFC 7386 JSON Merge Patches. */
    template <typename values_argument_type_>
    status_t merge(values_argument_type_&& values,
                   ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return any_write(std::forward<values_argument_type_>(values), ustore_docs_modify_merge_k, this->type_,
                         durability);
    }

    /** Inserts @p values, failing where a document already exists. */
    template <typename values_argument_type_>
    status_t insert(values_argument_type_&& values,
                    ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return any_write(std::forward<values_argument_type_>(values), ustore_docs_modify_insert_k, this->type_,
                         durability);
    }

    /** Inserts @p values, replacing existing documents. */
    template <typename values_argument_type_>
    status_t upsert(values_argument_type_&& values,
                    ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return any_write(std::forward<values_argument_type_>(values), ustore_docs_modify_upsert_k, this->type_,
                         durability);
    }

    /** Replaces existing documents or fields with @p values. */
    template <typename values_argument_type_>
    status_t update(values_argument_type_&& values,
                    ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return any_write(std::forward<values_argument_type_>(values), ustore_docs_modify_update_k, this->type_,
                         durability);
    }

  protected:
    template <typename values_argument_type_>
    status_t any_write(values_argument_type_&& values_ref, ustore_docs_modification_t modification,
                       ustore_docs_field_type_t type, ustore_durability_t durability) noexcept {
        using values_t = std::decay_t<values_argument_type_>;

        decltype(auto) places = this->locations_store_.ref();
        strided_iterator<ustore_collection_id_t const> const collections = keys_extractor_t {}.collections(places);
        strided_iterator<ustore_key_t const> const keys = keys_extractor_t {}.keys(places);
        strided_strings<std::string_view> const fields {keys_extractor_t {}.fields(places)};

        values_t const& values = values_ref;
        values_arg_extractor<values_t> values_extractor;

        ustore_outcome_t outcome {};
        ustore_docs_write_t docs_write {};
        docs_write.database = this->receiver_.database;
        docs_write.executor = this->receiver_.executor;
        docs_write.outcome = &outcome;
        docs_write.transaction = this->receiver_.transaction;
        docs_write.count = keys_extractor_t {}.count(places);
        docs_write.collections = {collections.get(), collections.stride()};
        docs_write.keys = {keys.get(), keys.stride()};
        docs_write.fields = fields.view();
        docs_write.values = values_extractor.view(values);
        docs_write.type = type;
        docs_write.modification = modification;
        docs_write.durability = durability;

        ustore_docs_write(&docs_write);
        return status_t {outcome};
    }
};

static_assert(docs_read_ref<ustore_key_t>::is_one_k);
static_assert(std::is_same_v<docs_read_ref<ustore_key_t>::value_t, value_view_t>);
static_assert(!docs_read_ref<place_batch_t>::is_one_k);

} // namespace unum::ustore
