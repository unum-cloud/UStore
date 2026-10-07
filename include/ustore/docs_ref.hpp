/**
 *  @file include/ustore/docs_ref.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief Proxy reading and modifying a batch of documents or fields inside them.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::ptrdiff_t`

#include <span>        // `std::span`
#include <type_traits> // `std::conditional_t`, `std::decay_t`
#include <utility>     // `std::forward`, `std::move`

#include "ustore/docs.h"
#include "ustore/docs_table.hpp"  // `docs_table`, `table_header`, `table_header_view_t`
#include "ustore/ranges.hpp"      // `embedded_blobs_t`, `bits_span_t`, `joined_strs_t`, `owning_view`, `read_spans_t`
#include "ustore/ranges_args.hpp" // `location_store`, `places_arg_extractor`, `contents_arg_extractor`
#include "ustore/status.hpp"      // `status_t`, `expected`
#include "ustore/types.hpp"       // `arena_t`

namespace unum::ustore {

/**
 *  @brief Proxy reading and writing documents, or fields inside them, at a batch of locations.
 *
 *  A proxy assigns one value to one or many locations, or many values to as many locations.
 *  It allocates results in the arena of the collection that produced it, so threads sharing a collection,
 *  or reads interleaved with new requests, pass a dedicated @c arena_t through @c on.
 *  A batch result owns its presences, offsets or columns, while its tapes stay in that arena.
 *  Copying is cheap and nothing throws.
 *
 *  @tparam locations_ Addresses of the values: one key with an optional collection and field,
 *      an externally owned range of such addresses, or an array of them.
 */
template <typename locations_>
    requires(!std::is_rvalue_reference_v<locations_>)
class docs_ref {
  public:
    using locations_store_t = location_store<locations_>;
    using locations_plain_t = typename locations_store_t::plain_t;
    using keys_extractor_t = places_arg_extractor<locations_plain_t>;
    static constexpr bool is_one_k = keys_extractor_t::is_one_k;

    using value_t = std::conditional_t<is_one_k, value_view_t, owning_view<embedded_blobs_t>>;
    using present_t = std::conditional_t<is_one_k, bool, owning_view<bits_span_t>>;
    using length_t = std::conditional_t<is_one_k, ustore_length_t, owning_view<std::span<ustore_length_t>>>;

  protected:
    ustore_database_t database_ {nullptr};
    ustore_transaction_t transaction_ {nullptr};
    ustore_snapshot_t snapshot_ {};
    ustore_arena_t* arena_ {nullptr};
    locations_store_t locations_store_;
    ustore_doc_field_type_t type_ {ustore_doc_field_json_k};

  public:
    docs_ref(ustore_database_t database, ustore_transaction_t transaction, ustore_snapshot_t snapshot,
             locations_&& locations, ustore_arena_t* arena,
             ustore_doc_field_type_t type = ustore_doc_field_json_k) noexcept
        : database_(database), transaction_(transaction), snapshot_(snapshot), arena_(arena),
          locations_store_(std::forward<locations_>(locations)), type_(type) {}

    docs_ref& on(arena_t& arena) noexcept {
        arena_ = arena.member_ptr();
        return *this;
    }

    docs_ref& as(ustore_doc_field_type_t type) noexcept {
        type_ = type;
        return *this;
    }

    expected<value_t> value(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return any_get<value_t>(type_, watch);
    }

    expected<value_t> value(ustore_doc_field_type_t type, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return any_get<value_t>(type, watch);
    }

    operator expected<value_t>() noexcept { return value(); }

    expected<length_t> length(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return any_get<length_t>(type_, watch);
    }

    /** Checks which locations hold a value, which may be empty. */
    expected<present_t> present(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return any_get<present_t>(type_, watch);
    }

    /** Upserts @p contents pair-wise into the locations, in the format of the proxy. */
    template <typename contents_>
    status_t assign(contents_&& contents, ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return any_write(std::forward<contents_>(contents), ustore_doc_modify_upsert_k, type_, durability);
    }

    /** Upserts @p contents pair-wise into the locations, in the format @p type. */
    template <typename contents_>
    status_t assign(contents_&& contents, ustore_doc_field_type_t type,
                    ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return any_write(std::forward<contents_>(contents), ustore_doc_modify_upsert_k, type, durability);
    }

    /** Removes the documents whole, refusing locations that name a field, which a patch or a merge removes. */
    status_t erase(ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        decltype(auto) places = locations_store_.ref();
        ustore_size_t const count = keys_extractor_t {}.count(places);
        strided_iterator<ustore_collection_id_t const> const collections = keys_extractor_t {}.collections(places);
        strided_iterator<ustore_key_t const> const keys = keys_extractor_t {}.keys(places);
        strided_iterator<ustore_str_view_t const> const fields = keys_extractor_t {}.fields(places);
        for (std::size_t index = 0; fields && index != count; ++index)
            if (fields[static_cast<std::ptrdiff_t>(index)])
                return status_t {status_code_t::invalid_argument_k, "A field is removed by a patch or a merge"};

        ustore_outcome_t outcome {};
        ustore_remove_t remove {};
        remove.database = database_;
        remove.outcome = &outcome;
        remove.transaction = transaction_;
        remove.count = count;
        remove.collections = collections.get();
        remove.collections_stride = collections.stride();
        remove.keys = keys.get();
        remove.keys_stride = keys.stride();
        remove.durability = durability;
        ustore_remove(&remove);
        return status_t {outcome};
    }

    /** Keeps the keys, but empties the associated values. */
    status_t clear(ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return assign(value_view_t::make_empty(), durability);
    }

    locations_plain_t& locations() noexcept { return locations_store_.ref(); }
    locations_plain_t const& locations() const noexcept { return locations_store_.ref(); }

    /** Patches the documents with RFC 6902 JSON Patches. */
    template <typename contents_>
    status_t patch(contents_&& contents, ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return any_write(std::forward<contents_>(contents), ustore_doc_modify_patch_k, type_, durability);
    }

    /** Patches the documents with RFC 7386 JSON Merge Patches. */
    template <typename contents_>
    status_t merge(contents_&& contents, ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return any_write(std::forward<contents_>(contents), ustore_doc_modify_merge_k, type_, durability);
    }

    /** Inserts @p contents, failing where a document already exists. */
    template <typename contents_>
    status_t insert(contents_&& contents, ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return any_write(std::forward<contents_>(contents), ustore_doc_modify_insert_k, type_, durability);
    }

    /** Inserts @p contents, replacing existing documents. */
    template <typename contents_>
    status_t upsert(contents_&& contents, ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return any_write(std::forward<contents_>(contents), ustore_doc_modify_upsert_k, type_, durability);
    }

    /** Replaces existing documents or fields with @p contents. */
    template <typename contents_>
    status_t update(contents_&& contents, ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        return any_write(std::forward<contents_>(contents), ustore_doc_modify_update_k, type_, durability);
    }

    /** Lists the unique paths to the leaves of the documents. */
    expected<joined_strs_t> gist(ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        ustore_size_t found_count = 0;
        ustore_length_t* found_offsets = nullptr;
        ustore_char_t* found_fields = nullptr;

        decltype(auto) places = locations_store_.ref();
        strided_iterator<ustore_collection_id_t const> const collections = keys_extractor_t {}.collections(places);
        strided_iterator<ustore_key_t const> const keys = keys_extractor_t {}.keys(places);

        ustore_outcome_t outcome {};
        ustore_docs_gist_t docs_gist {};
        docs_gist.database = database_;
        docs_gist.outcome = &outcome;
        docs_gist.view = view_through(transaction_, snapshot_, watch);
        docs_gist.count = keys_extractor_t {}.count(places);
        docs_gist.collections = collections.get();
        docs_gist.collections_stride = collections.stride();
        docs_gist.keys = keys.get();
        docs_gist.keys_stride = keys.stride();
        docs_gist.fields_count = &found_count;
        docs_gist.offsets = &found_offsets;
        docs_gist.fields = &found_fields;
        docs_gist.arena = arena_;

        ustore_docs_gist(&docs_gist);
        if (outcome.status != ustore_status_success_k) return status_t {outcome};
        return joined_strs_t {found_count, found_offsets, found_fields};
    }

    /** Gathers every column of @p header from every document into a table typed at runtime. */
    expected<owning_view<docs_table_t>> gather(table_header_view_t const& header,
                                               ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return any_gather<docs_table_t>(header, watch);
    }

    /** Gathers every column of @p header from every document into a table typed at compile time. */
    template <typename... column_types_>
    expected<owning_view<docs_table<column_types_...>>> gather(table_header<column_types_...> const& header,
                                                               ustore_watch_t watch = ustore_watch_keys_k) noexcept {
        return any_gather<docs_table<column_types_...>>(header, watch);
    }

  protected:
    template <typename contents_>
    status_t any_write(contents_&& contents, ustore_doc_modification_t modification, ustore_doc_field_type_t type,
                       ustore_durability_t durability) noexcept {
        using values_t = std::decay_t<contents_>;
        using value_extractor_t = contents_arg_extractor<values_t>;

        decltype(auto) places = locations_store_.ref();
        strided_iterator<ustore_collection_id_t const> const collections = keys_extractor_t {}.collections(places);
        strided_iterator<ustore_key_t const> const keys = keys_extractor_t {}.keys(places);
        strided_iterator<ustore_str_view_t const> const fields = keys_extractor_t {}.fields(places);

        values_t const& values = contents;
        strided_iterator<ustore_bytes_cptr_t const> const values_contents = value_extractor_t {}.contents(values);
        strided_iterator<ustore_length_t const> const values_starts = value_extractor_t {}.starts(values);
        strided_iterator<ustore_length_t const> const values_lengths = value_extractor_t {}.lengths(values);

        ustore_outcome_t outcome {};
        ustore_docs_write_t docs_write {};
        docs_write.database = database_;
        docs_write.outcome = &outcome;
        docs_write.transaction = transaction_;
        docs_write.count = keys_extractor_t {}.count(places);
        docs_write.collections = collections.get();
        docs_write.collections_stride = collections.stride();
        docs_write.keys = keys.get();
        docs_write.keys_stride = keys.stride();
        docs_write.fields = fields.get();
        docs_write.fields_stride = fields.stride();
        docs_write.starts = values_starts.get();
        docs_write.starts_stride = values_starts.stride();
        docs_write.lengths = values_lengths.get();
        docs_write.lengths_stride = values_lengths.stride();
        docs_write.values = values_contents.get();
        docs_write.values_stride = values_contents.stride();
        docs_write.type = type;
        docs_write.modification = modification;
        docs_write.durability = durability;

        ustore_docs_write(&docs_write);
        return status_t {outcome};
    }

    template <typename expected_ = value_t>
    expected<expected_> any_get(ustore_doc_field_type_t type, ustore_watch_t watch) noexcept {
        decltype(auto) places = locations_store_.ref();
        ustore_size_t const count = keys_extractor_t {}.count(places);
        strided_iterator<ustore_collection_id_t const> const collections = keys_extractor_t {}.collections(places);
        strided_iterator<ustore_key_t const> const keys = keys_extractor_t {}.keys(places);
        strided_iterator<ustore_str_view_t const> const fields = keys_extractor_t {}.fields(places);

        // A single location reads into the stack, and a batch into one buffer its result owns
        alignas(ustore_length_t) std::byte single[read_spans_t::bytes(1)] {};
        st::basic_vector<std::byte> batch;
        if constexpr (!is_one_k)
            if (st::failed(batch.resize(read_spans_t::bytes(count))))
                return status_t {status_code_t::out_of_memory_k, "Failed to allocate the spans of a read"};
        read_spans_t const spans = read_spans_t::carve(is_one_k ? single : batch.data(), count);

        ustore_byte_t* found_values = nullptr;
        ustore_outcome_t outcome {};
        ustore_docs_read_t docs_read {};
        docs_read.database = database_;
        docs_read.outcome = &outcome;
        docs_read.view = view_through(transaction_, snapshot_, watch);
        docs_read.count = count;
        docs_read.collections = collections.get();
        docs_read.collections_stride = collections.stride();
        docs_read.keys = keys.get();
        docs_read.keys_stride = keys.stride();
        docs_read.fields = fields.get();
        docs_read.fields_stride = fields.stride();
        docs_read.presences = spans.presences;
        docs_read.offsets = spans.offsets;
        docs_read.values = std::is_same_v<expected_, value_t> ? &found_values : nullptr;
        docs_read.arena = arena_;
        docs_read.type = type;

        ustore_docs_read(&docs_read);
        if (outcome.status != ustore_status_success_k) return status_t {outcome};
        spans.derive_lengths(count);

        if constexpr (std::is_same_v<expected_, length_t>) {
            if constexpr (is_one_k) return ustore_length_t {spans.lengths[0]};
            else return length_t {std::span<ustore_length_t> {spans.lengths, count}, std::move(batch)};
        }
        else if constexpr (std::is_same_v<expected_, present_t>) {
            if constexpr (is_one_k) return bool(bits_span_t {spans.presences}[0]);
            else return present_t {bits_span_t {spans.presences}, std::move(batch)};
        }
        else {
            embedded_blobs_t const many {count, spans.offsets, spans.lengths, found_values};
            if constexpr (is_one_k) return value_view_t {many[0]};
            else return value_t {many, std::move(batch)};
        }
    }

    template <typename table_, typename header_>
    expected<owning_view<table_>> any_gather(header_ const& header, ustore_watch_t watch) noexcept {
        decltype(auto) places = locations_store_.ref();
        ustore_size_t const count = keys_extractor_t {}.count(places);
        strided_iterator<ustore_collection_id_t const> const collections = keys_extractor_t {}.collections(places);
        strided_iterator<ustore_key_t const> const keys = keys_extractor_t {}.keys(places);
        strided_range<ustore_str_view_t const> const fields = header.fields();
        strided_range<ustore_doc_field_type_t const> const types = header.types();

        expected<owning_view<table_>> table = table_::make(count, fields.size(), collections, keys, fields.begin(),
                                                           types.begin());
        if (!table) return table;

        ustore_outcome_t outcome {};
        ustore_docs_gather_t docs_gather {};
        docs_gather.database = database_;
        docs_gather.outcome = &outcome;
        docs_gather.view = view_through(transaction_, snapshot_, watch);
        docs_gather.count = count;
        docs_gather.fields_count = fields.size();
        docs_gather.collections = collections.get();
        docs_gather.collections_stride = collections.stride();
        docs_gather.keys = keys.get();
        docs_gather.keys_stride = keys.stride();
        docs_gather.fields = fields.data();
        docs_gather.fields_stride = fields.stride();
        docs_gather.types = types.data();
        docs_gather.types_stride = types.stride();
        docs_gather.columns_validities = table->columns_validities();
        docs_gather.columns_conversions = table->columns_conversions();
        docs_gather.columns_collisions = table->columns_collisions();
        docs_gather.columns_scalars = table->columns_scalars();
        docs_gather.columns_offsets = table->columns_offsets();
        docs_gather.columns_lengths = table->columns_lengths();
        docs_gather.joined_strings = table->member_tape();
        docs_gather.arena = arena_;

        ustore_docs_gather(&docs_gather);
        if (outcome.status != ustore_status_success_k) return status_t {outcome};
        return table;
    }
};

static_assert(docs_ref<ustore_key_t>::is_one_k);
static_assert(std::is_same_v<docs_ref<ustore_key_t>::value_t, value_view_t>);
static_assert(!docs_ref<places_arg_t>::is_one_k);

} // namespace unum::ustore
