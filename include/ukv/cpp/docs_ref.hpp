/**
 * @file docs_ref.hpp
 * @author Ashot Vardanian
 * @date 26 Jun 2022
 * @addtogroup Cpp
 *
 * @brief C++ bindings for "ukv/db.h".
 */

#pragma once
#include "ukv/ukv.h"
#include "ukv/cpp/types.hpp"      // `arena_t`
#include "ukv/cpp/status.hpp"     // `status_t`
#include "ukv/cpp/sfinae.hpp"     // `location_store_gt`
#include "ukv/cpp/docs_table.hpp" // `docs_table_t`

namespace unum::ukv {

template <typename locations_store_t>
class docs_ref_gt;

/**
 * @brief A proxy object, that allows both lookups and writes
 * with `[]` and assignment operators for a batch of keys
 * and @b sub-keys/fields across different documents.
 * Following assignment combinations are possible:
 * - one value to many keys
 * - many values to many keys
 * - one value to one key
 * The only impossible combination is assigning many values to one key.
 *
 * @tparam locations_at Type describing the address of a value in DBMS.
 * - (ukv_collection_t?, ukv_key_t, ukv_field_t?): Single KV-pair location.
 * - (ukv_collection_t*, ukv_key_t*, ukv_field_t*): Externally owned range of keys.
 * - (ukv_collection_t[x], ukv_key_t[x], ukv_field_t[x]): On-stack array of addresses.
 *
 * ## Memory Management
 *
 * Every "container" that overloads the @b [] operator has an internal "arena",
 * that is shared between all the @c docs_ref_gt's produced from it. That will
 * work great, unless:
 * - multiple threads are working with same collection handle or transaction.
 * - reading responses interleaves with new requests, which gobbles temporary memory.
 * For those cases, you can create a separate @c arena_t and pass it to `.on(...)`
 * member function. In such HPC environments we would recommend to @b reuse one such
 * are on every thread.
 *
 * ## Class Specs
 *
 * - Copyable: Yes.
 * - Exceptions: Never.
 */
template <typename locations_at>
class docs_ref_gt {
  public:
    static_assert(!std::is_rvalue_reference_v<locations_at>, //
                  "The internal object can't be an R-value Reference");

    using locations_store_t = location_store_gt<locations_at>;
    using locations_plain_t = typename locations_store_t::plain_t;
    using keys_extractor_t = places_arg_extractor_gt<locations_plain_t>;
    static constexpr bool is_one_k = keys_extractor_t::is_one_k;

    using value_t = std::conditional_t<is_one_k, value_view_t, embedded_blobs_t>;
    using present_t = std::conditional_t<is_one_k, bool, bits_span_t>;
    using length_t = std::conditional_t<is_one_k, ukv_length_t, ptr_range_gt<ukv_length_t>>;

  protected:
    ukv_database_t db_ = nullptr;
    ukv_transaction_t transaction_ = nullptr;
    ukv_arena_t* arena_ = nullptr;
    locations_store_t locations_;
    ukv_doc_field_type_t type_ = ukv_doc_field_default_k;

    template <typename contents_arg_at>
    status_t any_write(contents_arg_at&&, ukv_doc_modification_t, ukv_doc_field_type_t, ukv_options_t) noexcept;

    template <typename expected_at = value_t>
    expected_gt<expected_at> any_get(ukv_doc_field_type_t, ukv_options_t) noexcept;

    template <typename expected_at, typename layout_at>
    expected_gt<expected_at> any_gather(layout_at&&, ukv_options_t) noexcept;

  public:
    docs_ref_gt(ukv_database_t db,
                ukv_transaction_t txn,
                locations_at&& locations,
                ukv_arena_t* arena,
                ukv_doc_field_type_t type = ukv_doc_field_default_k) noexcept
        : db_(db), transaction_(txn), arena_(arena), locations_(std::forward<locations_at>(locations)), type_(type) {}

    docs_ref_gt(docs_ref_gt&&) = default;
    docs_ref_gt& operator=(docs_ref_gt&&) = default;
    docs_ref_gt(docs_ref_gt const&) = default;
    docs_ref_gt& operator=(docs_ref_gt const&) = default;

    docs_ref_gt& on(arena_t& arena) noexcept {
        arena_ = arena.member_ptr();
        return *this;
    }

    docs_ref_gt& as(ukv_doc_field_type_t type) noexcept {
        type_ = type;
        return *this;
    }

    expected_gt<value_t> value(bool watch = true) noexcept {
        return any_get<value_t>(type_, !watch ? ukv_option_transaction_dont_watch_k : ukv_options_default_k);
    }

    expected_gt<value_t> value(ukv_doc_field_type_t type, bool watch = true) noexcept {
        return any_get<value_t>(type, !watch ? ukv_option_transaction_dont_watch_k : ukv_options_default_k);
    }

    operator expected_gt<value_t>() noexcept { return value(); }

    expected_gt<length_t> length(bool watch = true) noexcept {
        return any_get<length_t>(type_, !watch ? ukv_option_transaction_dont_watch_k : ukv_options_default_k);
    }

    /**
     * @brief Checks if requested keys are present in the store.
     * ! Related values may be empty strings.
     */
    expected_gt<present_t> present(bool watch = true) noexcept {
        return any_get<present_t>(type_, !watch ? ukv_option_transaction_dont_watch_k : ukv_options_default_k);
    }

    /**
     * @brief Pair-wise assigns values to keys located in this proxy objects.
     * @param vals Values to be assigned.
     * @param flush Pass true, if you need the data to be persisted before returning.
     * @return status_t Non-NULL if only an error had occurred.
     */
    template <typename contents_arg_at>
    status_t assign(contents_arg_at&& vals, bool flush = false) noexcept {
        return any_write(std::forward<contents_arg_at>(vals),
                         ukv_doc_modify_upsert_k,
                         type_,
                         flush ? ukv_option_write_flush_k : ukv_options_default_k);
    }

    template <typename contents_arg_at>
    status_t assign(contents_arg_at&& vals, ukv_doc_field_type_t type, bool flush = false) noexcept {
        return any_write(std::forward<contents_arg_at>(vals),
                         ukv_doc_modify_upsert_k,
                         type,
                         flush ? ukv_option_write_flush_k : ukv_options_default_k);
    }

    /**
     * @brief Removes both the keys and the associated values.
     * @param flush Pass true, if you need the data to be persisted before returning.
     * @return status_t Non-NULL if only an error had occurred.
     */
    status_t erase(bool flush = false) noexcept { //
        return assign(nullptr, flush);
    }

    /**
     * @brief Keeps the keys, but clears the contents of associated values.
     * @param flush Pass true, if you need the data to be persisted before returning.
     * @return status_t Non-NULL if only an error had occurred.
     */
    status_t clear(bool flush = false) noexcept {
        ukv_bytes_ptr_t any = reinterpret_cast<ukv_bytes_ptr_t>(this);
        ukv_length_t len = 0;
        contents_arg_t arg {
            .offsets_begin = {},
            .lengths_begin = {&len},
            .contents_begin = {&any},
            .count = 1,
        };
        return assign(arg, flush);
    }

    template <typename contents_arg_at>
    docs_ref_gt& operator=(contents_arg_at&& vals) noexcept(false) {
        auto status = assign(std::forward<contents_arg_at>(vals));
        status.throw_unhandled();
        return *this;
    }

    docs_ref_gt& operator=(std::nullptr_t) noexcept(false) {
        auto status = erase();
        status.throw_unhandled();
        return *this;
    }

    locations_plain_t& locations() noexcept { return locations_.ref(); }
    locations_plain_t& locations() const noexcept { return locations_.ref(); }

    /**
     * @brief Patches hierarchical documents with RFC 6902 JSON Patches.
     */
    template <typename contents_arg_at>
    status_t patch(contents_arg_at&& vals, bool flush = false) noexcept {
        return any_write(std::forward<contents_arg_at>(vals),
                         ukv_doc_modify_patch_k,
                         type_,
                         flush ? ukv_option_write_flush_k : ukv_options_default_k);
    }

    /**
     * @brief Patches hierarchical documents with RFC 7386 JSON Merge Patches.
     */
    template <typename contents_arg_at>
    status_t merge(contents_arg_at&& vals, bool flush = false) noexcept {
        return any_write(std::forward<contents_arg_at>(vals),
                         ukv_doc_modify_merge_k,
                         type_,
                         flush ? ukv_option_write_flush_k : ukv_options_default_k);
    }

    template <typename contents_arg_at>
    status_t insert(contents_arg_at&& vals, bool flush = false) noexcept {
        return any_write(std::forward<contents_arg_at>(vals),
                         ukv_doc_modify_insert_k,
                         type_,
                         flush ? ukv_option_write_flush_k : ukv_options_default_k);
    }

    template <typename contents_arg_at>
    status_t upsert(contents_arg_at&& vals, bool flush = false) noexcept {
        return any_write(std::forward<contents_arg_at>(vals),
                         ukv_doc_modify_upsert_k,
                         type_,
                         flush ? ukv_option_write_flush_k : ukv_options_default_k);
    }

    template <typename contents_arg_at>
    status_t update(contents_arg_at&& vals, bool flush = false) noexcept {
        return any_write(std::forward<contents_arg_at>(vals),
                         ukv_doc_modify_update_k,
                         type_,
                         flush ? ukv_option_write_flush_k : ukv_options_default_k);
    }

    /**
     * @brief Find the names of all unique fields in requested documents.
     */
    expected_gt<joined_strs_t> gist(bool watch = true) noexcept;

    /**
     * @brief For N documents and M fields gather (N * M) responses.
     * You put in a @c table_layout_view_gt and you receive a @c `docs_table_gt`.
     * Any column type annotation is optional.
     */
    expected_gt<docs_table_t> gather(table_header_t const& header, bool watch = true) noexcept {
        auto options = !watch ? ukv_option_transaction_dont_watch_k : ukv_options_default_k;
        return any_gather<docs_table_t, table_header_t const&>(header, options);
    }

    expected_gt<docs_table_t> gather(table_header_view_t const& header, bool watch = true) noexcept {
        auto options = !watch ? ukv_option_transaction_dont_watch_k : ukv_options_default_k;
        return any_gather<docs_table_t, table_header_view_t const&>(header, options);
    }

    template <typename... column_types_at>
    expected_gt<docs_table_gt<column_types_at...>> gather( //
        table_header_gt<column_types_at...> const& header,
        bool watch = true) noexcept {
        auto options = !watch ? ukv_option_transaction_dont_watch_k : ukv_options_default_k;
        using input_t = table_header_gt<column_types_at...>;
        using output_t = docs_table_gt<column_types_at...>;
        return any_gather<output_t, input_t const&>(header, options);
    }
};

static_assert(docs_ref_gt<ukv_key_t>::is_one_k);
static_assert(std::is_same<docs_ref_gt<ukv_key_t>::value_t, value_view_t>());
static_assert(docs_ref_gt<ukv_key_t>::is_one_k);
static_assert(!docs_ref_gt<places_arg_t>::is_one_k);

template <typename locations_at>
template <typename expected_at>
expected_gt<expected_at> docs_ref_gt<locations_at>::any_get(ukv_doc_field_type_t type, ukv_options_t options) noexcept {

    status_t status;
    ukv_length_t* found_offsets = nullptr;
    ukv_length_t* found_lengths = nullptr;
    ukv_bytes_ptr_t found_values = nullptr;
    ukv_octet_t* found_presences = nullptr;
    constexpr bool wants_value = std::is_same<value_t, expected_at>();
    constexpr bool wants_length = std::is_same<length_t, expected_at>();
    constexpr bool wants_present = std::is_same<present_t, expected_at>();

    decltype(auto) locs = locations_.ref();
    auto count = keys_extractor_t {}.count(locs);
    auto keys = keys_extractor_t {}.keys(locs);
    auto collections = keys_extractor_t {}.collections(locs);
    auto fields = keys_extractor_t {}.fields(locs);
    auto has_fields = fields && (!fields.repeats() || *fields);

    ukv_docs_read_t docs_read {};
    docs_read.db = db_;
    docs_read.error = status.member_ptr();
    docs_read.transaction = transaction_;
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
    docs_read.presences = wants_present ? &found_presences : nullptr;
    docs_read.offsets = wants_value ? &found_offsets : nullptr;
    docs_read.lengths = wants_value || wants_length ? &found_lengths : nullptr;
    docs_read.values = wants_value ? &found_values : nullptr;

    ukv_docs_read(&docs_read);

    if (!status)
        return std::move(status);

    if constexpr (wants_length) {
        ptr_range_gt<ukv_length_t> many {found_lengths, found_lengths + count};
        if constexpr (is_one_k)
            return many[0];
        else
            return many;
    }
    else if constexpr (wants_present) {
        bits_span_t many {found_presences};
        if constexpr (is_one_k)
            return bool(many[0]);
        else
            return many;
    }
    else {
        embedded_blobs_t many {count, found_offsets, found_lengths, found_values};
        if constexpr (is_one_k)
            return many[0];
        else
            return many;
    }
}

template <typename locations_at>
template <typename contents_arg_at>
status_t docs_ref_gt<locations_at>::any_write(contents_arg_at&& vals_ref,
                                              ukv_doc_modification_t modification,
                                              ukv_doc_field_type_t type,
                                              ukv_options_t options) noexcept {
    status_t status;
    using value_extractor_t = contents_arg_extractor_gt<std::remove_reference_t<contents_arg_at>>;

    decltype(auto) locs = locations_.ref();
    auto count = keys_extractor_t {}.count(locs);
    auto keys = keys_extractor_t {}.keys(locs);
    auto collections = keys_extractor_t {}.collections(locs);
    auto fields = keys_extractor_t {}.fields(locs);

    auto vals = vals_ref;
    auto contents = value_extractor_t {}.contents(vals);
    auto offsets = value_extractor_t {}.offsets(vals);
    auto lengths = value_extractor_t {}.lengths(vals);

    ukv_docs_write_t docs_write {};
    docs_write.db = db_;
    docs_write.error = status.member_ptr();
    docs_write.transaction = transaction_;
    docs_write.arena = arena_;
    docs_write.options = options;
    docs_write.type = type;
    docs_write.modification = modification;
    docs_write.tasks_count = count;
    docs_write.collections = collections.get();
    docs_write.collections_stride = collections.stride();
    docs_write.keys = keys.get();
    docs_write.keys_stride = keys.stride();
    docs_write.fields = fields.get();
    docs_write.fields_stride = fields.stride();
    docs_write.offsets = offsets.get();
    docs_write.offsets_stride = offsets.stride();
    docs_write.lengths = lengths.get();
    docs_write.lengths_stride = lengths.stride();
    docs_write.values = contents.get();
    docs_write.values_stride = contents.stride();

    ukv_docs_write(&docs_write);

    return status;
}

template <typename locations_at>
expected_gt<joined_strs_t> docs_ref_gt<locations_at>::gist(bool watch) noexcept {

    status_t status;
    ukv_size_t found_count = 0;
    ukv_length_t* found_offsets = nullptr;
    ukv_str_span_t found_strings = nullptr;

    auto options = !watch ? ukv_option_transaction_dont_watch_k : ukv_options_default_k;
    decltype(auto) locs = locations_.ref();
    auto count = keys_extractor_t {}.count(locs);
    auto keys = keys_extractor_t {}.keys(locs);
    auto collections = keys_extractor_t {}.collections(locs);

    ukv_docs_gist_t docs_gist {};
    docs_gist.db = db_;
    docs_gist.error = status.member_ptr();
    docs_gist.transaction = transaction_;
    docs_gist.arena = arena_;
    docs_gist.options = options;
    docs_gist.docs_count = count;
    docs_gist.collections = collections.get();
    docs_gist.collections_stride = collections.stride();
    docs_gist.keys = keys.get();
    docs_gist.keys_stride = keys.stride();
    docs_gist.fields_count = &found_count;
    docs_gist.offsets = &found_offsets;
    docs_gist.fields = &found_strings;

    ukv_docs_gist(&docs_gist);

    joined_strs_t view {found_count, found_offsets, found_strings};
    return {std::move(status), std::move(view)};
}

template <typename locations_at>
template <typename expected_at, typename layout_at>
expected_gt<expected_at> docs_ref_gt<locations_at>::any_gather(layout_at&& layout, ukv_options_t options) noexcept {

    decltype(auto) locs = locations_.ref();
    auto count = keys_extractor_t {}.count(locs);
    auto keys = keys_extractor_t {}.keys(locs);
    auto collections = keys_extractor_t {}.collections(locs);

    status_t status;
    expected_at view {
        count,
        layout.fields().size(),
        collections,
        keys,
        layout.fields().begin(),
        layout.types().begin(),
    };

    ukv_docs_gather_t docs_gather {};
    docs_gather.db = db_;
    docs_gather.error = status.member_ptr();
    docs_gather.transaction = transaction_;
    docs_gather.arena = arena_;
    docs_gather.options = options;
    docs_gather.docs_count = count;
    docs_gather.fields_count = layout.fields().size();
    docs_gather.collections = collections.get();
    docs_gather.collections_stride = collections.stride();
    docs_gather.keys = keys.get();
    docs_gather.keys_stride = keys.stride();
    docs_gather.fields = layout.fields().begin().get();
    docs_gather.fields_stride = layout.fields().stride();
    docs_gather.types = layout.types().begin().get();
    docs_gather.types_stride = layout.types().stride();
    docs_gather.columns_validities = view.member_validities();
    docs_gather.columns_conversions = view.member_conversions();
    docs_gather.columns_collisions = view.member_collisions();
    docs_gather.columns_scalars = view.member_scalars();
    docs_gather.columns_offsets = view.member_offsets();
    docs_gather.columns_lengths = view.member_lengths();
    docs_gather.joined_strings = view.member_tape();

    ukv_docs_gather(&docs_gather);

    return {std::move(status), std::move(view)};
}

} // namespace unum::ukv
