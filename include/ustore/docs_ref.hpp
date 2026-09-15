/**
 *  @file include/ustore/docs_ref.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief Proxy reading and modifying a batch of documents or fields inside them.
 */

#pragma once

#include <cstddef> // `std::nullptr_t`

#include <type_traits> // `std::conditional_t`, `std::decay_t`
#include <utility>     // `std::forward`, `std::move`

#include <ustore/docs.h>
#include <ustore/docs_table.hpp>  // `docs_table`, `table_header`, `table_header_view_t`
#include <ustore/ranges.hpp>      // `embedded_blobs_t`, `bits_span_t`, `joined_strs_t`, `ptr_range`
#include <ustore/ranges_args.hpp> // `location_store`, `places_arg_extractor`, `contents_arg_extractor`
#include <ustore/status.hpp>      // `status_t`, `expected`
#include <ustore/types.hpp>       // `arena_t`

namespace unum::ustore {

/**
 *  @brief Proxy reading and writing documents, or fields inside them, at a batch of locations.
 *
 *  A proxy assigns one value to one or many locations, or many values to as many locations.
 *  It allocates results in the arena of the collection that produced it, so threads sharing a collection,
 *  or reads interleaved with new requests, pass a dedicated @c arena_t through @c on.
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

    using value_t = std::conditional_t<is_one_k, value_view_t, embedded_blobs_t>;
    using present_t = std::conditional_t<is_one_k, bool, bits_span_t>;
    using length_t = std::conditional_t<is_one_k, ustore_length_t, ptr_range<ustore_length_t>>;

  protected:
    ustore_database_t db_ {nullptr};
    ustore_transaction_t transaction_ {nullptr};
    ustore_snapshot_t snapshot_ {};
    ustore_arena_t* arena_ {nullptr};
    locations_store_t locations_store_;
    ustore_doc_field_type_t type_ {ustore_doc_field_json_k};

  public:
    docs_ref(ustore_database_t db, ustore_transaction_t transaction, ustore_snapshot_t snapshot, locations_&& locations,
             ustore_arena_t* arena, ustore_doc_field_type_t type = ustore_doc_field_json_k) noexcept
        : db_(db), transaction_(transaction), snapshot_(snapshot), arena_(arena),
          locations_store_(std::forward<locations_>(locations)), type_(type) {}

    docs_ref& on(arena_t& arena) noexcept {
        arena_ = arena.member_ptr();
        return *this;
    }

    docs_ref& as(ustore_doc_field_type_t type) noexcept {
        type_ = type;
        return *this;
    }

    expected<value_t> value(ustore_options_t options = ustore_options_default_k) noexcept {
        return any_get<value_t>(type_, options);
    }

    expected<value_t> value(ustore_doc_field_type_t type,
                            ustore_options_t options = ustore_options_default_k) noexcept {
        return any_get<value_t>(type, options);
    }

    operator expected<value_t>() noexcept { return value(); }

    expected<length_t> length(ustore_options_t options = ustore_options_default_k) noexcept {
        return any_get<length_t>(type_, options);
    }

    /** Checks which locations hold a value, which may be empty. */
    expected<present_t> present(ustore_options_t options = ustore_options_default_k) noexcept {
        return any_get<present_t>(type_, options);
    }

    /** Upserts @p contents pair-wise into the locations, in the format of the proxy. */
    template <typename contents_>
    status_t assign(contents_&& contents, ustore_options_t options = ustore_options_default_k) noexcept {
        return any_write(std::forward<contents_>(contents), ustore_doc_modify_upsert_k, type_, options);
    }

    /** Upserts @p contents pair-wise into the locations, in the format @p type. */
    template <typename contents_>
    status_t assign(contents_&& contents, ustore_doc_field_type_t type,
                    ustore_options_t options = ustore_options_default_k) noexcept {
        return any_write(std::forward<contents_>(contents), ustore_doc_modify_upsert_k, type, options);
    }

    /** Removes the documents, or the fields inside them. */
    status_t erase(ustore_options_t options = ustore_options_default_k) noexcept { return assign(nullptr, options); }

    /** Keeps the keys, but empties the associated values. */
    status_t clear(ustore_options_t options = ustore_options_default_k) noexcept {
        ustore_bytes_ptr_t non_null = reinterpret_cast<ustore_bytes_ptr_t>(this);
        ustore_length_t length = 0;
        contents_arg_t empty {};
        empty.offsets_begin = {};
        empty.lengths_begin = {&length};
        empty.contents_begin = {&non_null};
        empty.count = 1;
        return assign(empty, options);
    }

    locations_plain_t& locations() noexcept { return locations_store_.ref(); }
    locations_plain_t const& locations() const noexcept { return locations_store_.ref(); }

    /** Patches the documents with RFC 6902 JSON Patches. */
    template <typename contents_>
    status_t patch(contents_&& contents, ustore_options_t options = ustore_options_default_k) noexcept {
        return any_write(std::forward<contents_>(contents), ustore_doc_modify_patch_k, type_, options);
    }

    /** Patches the documents with RFC 7386 JSON Merge Patches. */
    template <typename contents_>
    status_t merge(contents_&& contents, ustore_options_t options = ustore_options_default_k) noexcept {
        return any_write(std::forward<contents_>(contents), ustore_doc_modify_merge_k, type_, options);
    }

    /** Inserts @p contents, failing where a document already exists. */
    template <typename contents_>
    status_t insert(contents_&& contents, ustore_options_t options = ustore_options_default_k) noexcept {
        return any_write(std::forward<contents_>(contents), ustore_doc_modify_insert_k, type_, options);
    }

    /** Inserts @p contents, replacing existing documents. */
    template <typename contents_>
    status_t upsert(contents_&& contents, ustore_options_t options = ustore_options_default_k) noexcept {
        return any_write(std::forward<contents_>(contents), ustore_doc_modify_upsert_k, type_, options);
    }

    /** Replaces existing documents or fields with @p contents. */
    template <typename contents_>
    status_t update(contents_&& contents, ustore_options_t options = ustore_options_default_k) noexcept {
        return any_write(std::forward<contents_>(contents), ustore_doc_modify_update_k, type_, options);
    }

    /** Lists the unique paths to the leaves of the documents. */
    expected<joined_strs_t> gist(ustore_options_t options = ustore_options_default_k) noexcept {
        status_t status;
        ustore_size_t found_count = 0;
        ustore_length_t* found_offsets = nullptr;
        ustore_str_span_t found_strings = nullptr;

        decltype(auto) places = locations_store_.ref();
        auto const collections = keys_extractor_t {}.collections(places);
        auto const keys = keys_extractor_t {}.keys(places);

        ustore_docs_gist_t docs_gist {};
        docs_gist.db = db_;
        docs_gist.error = status.member_ptr();
        docs_gist.transaction = transaction_;
        docs_gist.snapshot = snapshot_;
        docs_gist.arena = arena_;
        docs_gist.options = options;
        docs_gist.docs_count = keys_extractor_t {}.count(places);
        docs_gist.collections = collections.get();
        docs_gist.collections_stride = collections.stride();
        docs_gist.keys = keys.get();
        docs_gist.keys_stride = keys.stride();
        docs_gist.fields_count = &found_count;
        docs_gist.offsets = &found_offsets;
        docs_gist.fields = &found_strings;

        ustore_docs_gist(&docs_gist);

        joined_strs_t view {found_count, found_offsets, found_strings};
        return {std::move(status), std::move(view)};
    }

    /** Gathers every column of @p header from every document into a table typed at runtime. */
    expected<docs_table_t> gather(table_header_view_t const& header,
                                  ustore_options_t options = ustore_options_default_k) noexcept {
        return any_gather<docs_table_t>(header, options);
    }

    /** Gathers every column of @p header from every document into a table typed at compile time. */
    template <typename... column_types_>
    expected<docs_table<column_types_...>> gather(table_header<column_types_...> const& header,
                                                  ustore_options_t options = ustore_options_default_k) noexcept {
        return any_gather<docs_table<column_types_...>>(header, options);
    }

  protected:
    template <typename contents_>
    status_t any_write(contents_&& contents, ustore_doc_modification_t modification, ustore_doc_field_type_t type,
                       ustore_options_t options) noexcept {
        status_t status;
        using values_t = std::decay_t<contents_>;
        using value_extractor_t = contents_arg_extractor<values_t>;

        decltype(auto) places = locations_store_.ref();
        auto const collections = keys_extractor_t {}.collections(places);
        auto const keys = keys_extractor_t {}.keys(places);
        auto const fields = keys_extractor_t {}.fields(places);

        values_t const& values = contents;
        auto const values_contents = value_extractor_t {}.contents(values);
        auto const values_offsets = value_extractor_t {}.offsets(values);
        auto const values_lengths = value_extractor_t {}.lengths(values);

        ustore_docs_write_t docs_write {};
        docs_write.db = db_;
        docs_write.error = status.member_ptr();
        docs_write.transaction = transaction_;
        docs_write.arena = arena_;
        docs_write.options = options;
        docs_write.type = type;
        docs_write.modification = modification;
        docs_write.tasks_count = keys_extractor_t {}.count(places);
        docs_write.collections = collections.get();
        docs_write.collections_stride = collections.stride();
        docs_write.keys = keys.get();
        docs_write.keys_stride = keys.stride();
        docs_write.fields = fields.get();
        docs_write.fields_stride = fields.stride();
        docs_write.offsets = values_offsets.get();
        docs_write.offsets_stride = values_offsets.stride();
        docs_write.lengths = values_lengths.get();
        docs_write.lengths_stride = values_lengths.stride();
        docs_write.values = values_contents.get();
        docs_write.values_stride = values_contents.stride();

        ustore_docs_write(&docs_write);
        return status;
    }

    template <typename expected_ = value_t>
    expected<expected_> any_get(ustore_doc_field_type_t type, ustore_options_t options) noexcept {
        status_t status;
        ustore_length_t* found_offsets = nullptr;
        ustore_length_t* found_lengths = nullptr;
        ustore_bytes_ptr_t found_values = nullptr;
        ustore_octet_t* found_presences = nullptr;
        constexpr bool wants_value_k = std::is_same_v<value_t, expected_>;
        constexpr bool wants_length_k = std::is_same_v<length_t, expected_>;
        constexpr bool wants_present_k = std::is_same_v<present_t, expected_>;

        decltype(auto) places = locations_store_.ref();
        auto const count = keys_extractor_t {}.count(places);
        auto const collections = keys_extractor_t {}.collections(places);
        auto const keys = keys_extractor_t {}.keys(places);
        auto const fields = keys_extractor_t {}.fields(places);

        ustore_docs_read_t docs_read {};
        docs_read.db = db_;
        docs_read.error = status.member_ptr();
        docs_read.transaction = transaction_;
        docs_read.snapshot = snapshot_;
        docs_read.arena = arena_;
        docs_read.options = options;
        docs_read.type = type;
        docs_read.tasks_count = count;
        docs_read.collections = collections.get();
        docs_read.collections_stride = collections.stride();
        docs_read.keys = keys.get();
        docs_read.keys_stride = keys.stride();
        docs_read.fields = fields.get();
        docs_read.fields_stride = fields.stride();
        docs_read.presences = wants_present_k ? &found_presences : nullptr;
        docs_read.offsets = wants_value_k ? &found_offsets : nullptr;
        docs_read.lengths = wants_value_k || wants_length_k ? &found_lengths : nullptr;
        docs_read.values = wants_value_k ? &found_values : nullptr;

        ustore_docs_read(&docs_read);
        if (!status) return std::move(status);

        if constexpr (wants_length_k) {
            ptr_range<ustore_length_t> const many {found_lengths, found_lengths + count};
            if constexpr (is_one_k) return ustore_length_t {many[0]};
            else return ptr_range<ustore_length_t> {many};
        }
        else if constexpr (wants_present_k) {
            bits_span_t many {found_presences};
            if constexpr (is_one_k) return bool(many[0]);
            else return many;
        }
        else {
            embedded_blobs_t many {count, found_offsets, found_lengths, found_values};
            if constexpr (is_one_k) return many[0];
            else return many;
        }
    }

    template <typename table_, typename header_>
    expected<table_> any_gather(header_ const& header, ustore_options_t options) noexcept {
        decltype(auto) places = locations_store_.ref();
        auto const count = keys_extractor_t {}.count(places);
        auto const collections = keys_extractor_t {}.collections(places);
        auto const keys = keys_extractor_t {}.keys(places);

        status_t status;
        table_ view {
            count, header.fields().size(), collections, keys, header.fields().begin(), header.types().begin(),
        };

        ustore_docs_gather_t docs_gather {};
        docs_gather.db = db_;
        docs_gather.error = status.member_ptr();
        docs_gather.transaction = transaction_;
        docs_gather.snapshot = snapshot_;
        docs_gather.arena = arena_;
        docs_gather.options = options;
        docs_gather.docs_count = count;
        docs_gather.fields_count = header.fields().size();
        docs_gather.collections = collections.get();
        docs_gather.collections_stride = collections.stride();
        docs_gather.keys = keys.get();
        docs_gather.keys_stride = keys.stride();
        docs_gather.fields = header.fields().begin().get();
        docs_gather.fields_stride = header.fields().stride();
        docs_gather.types = header.types().begin().get();
        docs_gather.types_stride = header.types().stride();
        docs_gather.columns_validities = view.member_validities();
        docs_gather.columns_conversions = view.member_conversions();
        docs_gather.columns_collisions = view.member_collisions();
        docs_gather.columns_scalars = view.member_scalars();
        docs_gather.columns_offsets = view.member_offsets();
        docs_gather.columns_lengths = view.member_lengths();
        docs_gather.joined_strings = view.member_tape();

        ustore_docs_gather(&docs_gather);
        return {std::move(status), std::move(view)};
    }
};

static_assert(docs_ref<ustore_key_t>::is_one_k);
static_assert(std::is_same_v<docs_ref<ustore_key_t>::value_t, value_view_t>);
static_assert(!docs_ref<places_arg_t>::is_one_k);

} // namespace unum::ustore
