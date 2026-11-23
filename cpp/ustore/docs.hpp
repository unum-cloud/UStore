/**
 *  @file cpp/ustore/docs.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief Handles to a collection of JSON documents, the proxies reading and modifying documents or
 *      fields inside them, and the columnar tables a gather answers.
 */
#pragma once

#include <cstddef> // `std::size_t`, `std::ptrdiff_t`
#include <cstdint> // `std::int8_t`, `std::int16_t`, `std::int32_t`, `std::int64_t`, `std::uint8_t` & kin

#include <algorithm>        // `std::copy_n`
#include <array>            // `std::array`
#include <concepts>         // `std::same_as`
#include <initializer_list> // `std::initializer_list`
#include <iterator>         // `std::size`
#include <span>             // `std::span`
#include <string_view>      // `std::string_view`
#include <type_traits>      // `std::conditional_t`, `std::decay_t`, `std::is_rvalue_reference_v`
#include <utility>          // `std::forward`
#include <variant>          // `std::monostate`

#include "ustore/blobs.hpp" // `blobs_range_t`, `keys_range_t`, `keys_cursor`, `sample_keys_`, `measured_size_range_`
#include "ustore/types.hpp" // `arena_t`, `receiver_t`, `answered_`, `strided_strings`, `values_view_t`

namespace unum::ustore {

#pragma region Field Types

/** The document field type that stores @p element_type_, JSON for anything else. */
template <typename element_type_>
inline constexpr ustore_docs_field_type_t docs_field_type_of = ustore_docs_field_type_json_k;
template <>
inline constexpr ustore_docs_field_type_t docs_field_type_of<bool> = ustore_docs_field_type_bool_k;
template <>
inline constexpr ustore_docs_field_type_t docs_field_type_of<double> = ustore_docs_field_type_f64_k;
template <>
inline constexpr ustore_docs_field_type_t docs_field_type_of<float> = ustore_docs_field_type_f32_k;
template <>
inline constexpr ustore_docs_field_type_t docs_field_type_of<std::int64_t> = ustore_docs_field_type_i64_k;
template <>
inline constexpr ustore_docs_field_type_t docs_field_type_of<std::int32_t> = ustore_docs_field_type_i32_k;
template <>
inline constexpr ustore_docs_field_type_t docs_field_type_of<std::int16_t> = ustore_docs_field_type_i16_k;
template <>
inline constexpr ustore_docs_field_type_t docs_field_type_of<std::int8_t> = ustore_docs_field_type_i8_k;
template <>
inline constexpr ustore_docs_field_type_t docs_field_type_of<std::uint64_t> = ustore_docs_field_type_u64_k;
template <>
inline constexpr ustore_docs_field_type_t docs_field_type_of<std::uint32_t> = ustore_docs_field_type_u32_k;
template <>
inline constexpr ustore_docs_field_type_t docs_field_type_of<std::uint16_t> = ustore_docs_field_type_u16_k;
template <>
inline constexpr ustore_docs_field_type_t docs_field_type_of<std::uint8_t> = ustore_docs_field_type_u8_k;
template <>
inline constexpr ustore_docs_field_type_t docs_field_type_of<value_view_t> = ustore_docs_field_type_binary_k;
template <>
inline constexpr ustore_docs_field_type_t docs_field_type_of<std::string_view> = ustore_docs_field_type_string_k;
template <>
inline constexpr ustore_docs_field_type_t docs_field_type_of<char const*> = ustore_docs_field_type_string_k;

#pragma endregion Field Types

#pragma region Cells and Columns

/** Whether a gathered column of @p type holds strings, which offsets address,
 *  rather than scalars. */
constexpr bool is_variable_length(ustore_docs_field_type_t type) noexcept {
    return type == ustore_docs_field_type_string_k || type == ustore_docs_field_type_binary_k;
}

/** Element types stored as offsets into a joined tape rather than as fixed-width scalars. */
template <typename element_type_>
concept variable_length_element = std::same_as<element_type_, value_view_t> ||
                                  std::same_as<element_type_, std::string_view>;

/** One entry of a column, with the flags a gather reports for it. */
template <typename element_type_>
struct cell {

    /** Whether the value was found and converted. */
    bool valid {false};

    /** Whether the conversion changed the semantics of the value. */
    bool converted {false};

    /** Whether an inconvertible value sits under the field. */
    bool collides {false};

    /** The value, meaningful only when @c valid. */
    element_type_ value {};
};

/** Column of fixed-width scalars, viewing the arena of the gather. */
template <typename element_type_>
class scalar_column_view {
    ustore_docs_column_view_t arrays_ {};
    ustore_size_t count_ {0};
    std::string_view name_ {};

  public:
    using cell_t = cell<element_type_>;
    using value_type = cell_t;

    scalar_column_view(ustore_docs_column_view_t const& arrays, ustore_size_t count,
                       std::string_view name = {}) noexcept
        : arrays_(arrays), count_(count), name_(name) {}

    std::string_view name() const noexcept { return name_; }
    std::size_t size() const noexcept { return count_; }

    cell_t operator[](std::size_t index) const noexcept {
        element_type_ const* const scalars = reinterpret_cast<element_type_ const*>(arrays_.scalars);
        return {bits_view_t {arrays_.validities}[index], bits_view_t {arrays_.conversions}[index],
                bits_view_t {arrays_.collisions}[index], scalars[index]};
    }
};

/** Column of strings, a tape of either offset width inside the arena of the gather. */
template <typename element_type_>
class variable_length_column_view {
    ustore_docs_column_view_t arrays_ {};
    ustore_size_t count_ {0};
    std::string_view name_ {};
    ustore_docs_field_type_t type_ {ustore_docs_field_type_string_k};

  public:
    using cell_t = cell<element_type_>;
    using value_type = cell_t;

    variable_length_column_view(ustore_docs_column_view_t const& arrays, ustore_size_t count, std::string_view name,
                                ustore_docs_field_type_t type) noexcept
        : arrays_(arrays), count_(count), name_(name), type_(type) {}

    std::string_view name() const noexcept { return name_; }
    std::size_t size() const noexcept { return count_; }

    cell_t operator[](std::size_t index) const noexcept {
        std::string_view text = strings_view_t {arrays_.strings, count_}[index];
        // A text column spans the null terminator of every value, which the cell leaves out
        if (type_ == ustore_docs_field_type_string_k && !text.empty()) text.remove_suffix(1);
        return {bits_view_t {arrays_.validities}[index], bits_view_t {arrays_.conversions}[index],
                bits_view_t {arrays_.collisions}[index], element_type_ {text}};
    }
};

/** Column view matching how @p element_type_ is stored. */
template <typename element_type_>
using column_view = std::conditional_t<variable_length_element<element_type_>,
                                       variable_length_column_view<element_type_>, scalar_column_view<element_type_>>;

/** Column whose type is known only at runtime, viewed as a typed column through @c as. */
class dynamic_column_view_t {
    ustore_docs_column_view_t arrays_ {};
    ustore_size_t count_ {0};
    std::string_view name_ {};
    ustore_docs_field_type_t type_ {ustore_docs_field_type_json_k};

  public:
    dynamic_column_view_t(ustore_docs_column_view_t const& arrays, ustore_size_t count, std::string_view name = {},
                          ustore_docs_field_type_t type = ustore_docs_field_type_json_k) noexcept
        : arrays_(arrays), count_(count), name_(name), type_(type) {}

    std::string_view name() const noexcept { return name_; }
    ustore_docs_field_type_t type() const noexcept { return type_; }
    std::size_t size() const noexcept { return count_; }

    template <typename element_type_>
    column_view<element_type_> as() const noexcept {
        if constexpr (variable_length_element<element_type_>) return {arrays_, count_, name_, type_};
        else return {arrays_, count_, name_};
    }

    ustore_octet_t const* validities() const noexcept { return arrays_.validities; }

    /** The column as the gather wrote it, which an Arrow export reads. */
    ustore_docs_column_view_t const& arrays() const noexcept { return arrays_; }

    /** The scalars of a scalar column, or null for a string column. */
    ustore_byte_t const* scalars() const noexcept { return arrays_.scalars; }

    /** The strings of a string column as a tape, or a zeroed view for a scalar column. */
    ustore_strings_view_t const& strings() const noexcept { return arrays_.strings; }
};

#pragma endregion Cells and Columns

#pragma region Headers

/** The first column of a table, naming the document of every row. */
struct table_index_view_t {
    strided_iterator<ustore_collection_id_t const> collections_begin;
    strided_iterator<ustore_key_t const> keys_begin;
    std::size_t count {0};

    strided_span<ustore_collection_id_t const> collections() const noexcept { return {collections_begin, count}; }
    strided_span<ustore_key_t const> keys() const noexcept { return {keys_begin, count}; }
};

/** The header row of a table, naming the field and type of every column, each back to back, as a
 *  gather borrows them. */
struct table_header_view_t {
    std::string_view const* fields_begin {nullptr};
    ustore_docs_field_type_t const* types_begin {nullptr};
    std::size_t count {0};

    strided_span<std::string_view const> fields() const noexcept {
        return {{fields_begin, sizeof(std::string_view)}, count};
    }
    strided_span<ustore_docs_field_type_t const> types() const noexcept {
        return {{types_begin, sizeof(ustore_docs_field_type_t)}, count};
    }
};

/** Header of a table whose column types are known at compile time, built column by column
 *  through @c with. */
template <typename... column_types_>
struct table_header {

    /** Field of every column, in order. */
    std::array<std::string_view, sizeof...(column_types_)> names;

    /** Type every column converts its field into, in order, back to back as a gather reads them. */
    std::array<ustore_docs_field_type_t, sizeof...(column_types_)> types_of {docs_field_type_of<column_types_>...};

    template <typename element_type_>
    table_header<column_types_..., element_type_> with(std::string_view name) && noexcept {
        table_header<column_types_..., element_type_> extended;
        std::copy_n(names.begin(), names.size(), extended.names.begin());
        extended.names.back() = name;
        return extended;
    }

    table_header_view_t view() const noexcept { return {names.data(), types_of.data(), names.size()}; }
    strided_span<std::string_view const> fields() const noexcept { return view().fields(); }
    strided_span<ustore_docs_field_type_t const> types() const noexcept { return view().types(); }
};

/** Starts a header whose column types are known at compile time. */
inline table_header<> make_table_header() noexcept { return {}; }

/** Views @p fields, whose types @p types name only at runtime, one per field, as a table header. */
inline table_header_view_t make_table_header(std::span<std::string_view const> fields,
                                             std::span<ustore_docs_field_type_t const> types) noexcept {
    return {fields.data(), types.data(), fields.size()};
}

#pragma endregion Headers

#pragma region Tables

/** The type of column @p index_ among @p column_types_. */
template <std::size_t index_, typename head_type_, typename... tail_types_>
struct nth_column_type {
    using type = typename nth_column_type<index_ - 1, tail_types_...>::type;
};

template <typename head_type_, typename... tail_types_>
struct nth_column_type<0, head_type_, tail_types_...> {
    using type = head_type_;
};

/**
 *  @brief Results of a gather, with documents as rows and fields as columns.
 *
 *  A view that copies cheaply: its columns live in the arena of the gather, while its index and
 *  header view the keys and fields the gather was asked for.
 *
 *  @tparam column_types_ Types of the columns, or @c std::monostate alone when they are known
 *      only at runtime.
 */
template <typename... column_types_>
class docs_table {
    static constexpr bool is_dynamic_k = std::is_same_v<docs_table, docs_table<std::monostate>>;

    template <std::size_t index_>
    using column_of = std::conditional_t<is_dynamic_k, //
                                         dynamic_column_view_t,
                                         column_view<typename nth_column_type<index_, column_types_...>::type>>;

    /** What @c column answers read as @p element_type_, or unread under @c std::monostate. */
    template <typename element_type_>
    using typed_column_of = std::conditional_t<std::is_same_v<element_type_, std::monostate>, dynamic_column_view_t,
                                               column_view<element_type_>>;

    ustore_size_t docs_count_ {0};
    ustore_size_t fields_count_ {0};

    strided_iterator<ustore_collection_id_t const> collections_;
    strided_iterator<ustore_key_t const> keys_;
    strided_iterator<std::string_view const> fields_;
    strided_iterator<ustore_docs_field_type_t const> types_;

    /** One column per field, inside the arena of the gather. */
    ustore_docs_column_view_t const* columns_ {nullptr};

  public:
    docs_table() noexcept = default;
    docs_table( //
        ustore_size_t docs_count, ustore_size_t fields_count,
        strided_iterator<ustore_collection_id_t const> collections, strided_iterator<ustore_key_t const> keys,
        strided_iterator<std::string_view const> fields, strided_iterator<ustore_docs_field_type_t const> types,
        ustore_docs_column_view_t const* columns) noexcept
        : docs_count_(docs_count), fields_count_(fields_count), collections_(collections), keys_(keys), fields_(fields),
          types_(types), columns_(columns) {}

    table_index_view_t index() const noexcept { return {collections_, keys_, docs_count_}; }
    table_header_view_t header() const noexcept { return {fields_.get(), types_.get(), fields_count_}; }

    template <typename element_type_ = std::monostate>
    typed_column_of<element_type_> column(std::size_t index) const noexcept {
        std::ptrdiff_t const signed_index = static_cast<std::ptrdiff_t>(index);
        dynamic_column_view_t const punned {columns_[index], docs_count_, fields_[signed_index], types_[signed_index]};
        if constexpr (std::is_same_v<element_type_, std::monostate>) return punned;
        else return punned.template as<element_type_>();
    }

    template <std::size_t index_>
    column_of<index_> column() const noexcept {
        if constexpr (is_dynamic_k) return column(index_);
        else return this->template column<typename nth_column_type<index_, column_types_...>::type>(index_);
    }

    std::size_t rows() const noexcept { return docs_count_; }
    std::size_t columns() const noexcept { return fields_count_; }
};

using docs_table_t = docs_table<std::monostate>;

/** Where a gather writes its columns, beside the rows and the header its table views. */
template <typename... column_types_>
struct outputs<docs_table<column_types_...>> {

    /** Documents gathered, one per row. */
    ustore_size_t rows = 0;

    /** The collection and key of every row, as the gather was asked for them. */
    strided_iterator<ustore_collection_id_t const> collections;
    strided_iterator<ustore_key_t const> keys;

    /** The field and type of every column, as the gather was asked for them. */
    table_header_view_t header {};

    /** Output for one column per field, inside the arena. */
    ustore_docs_column_view_t const* columns = nullptr;

    docs_table<column_types_...> answer() const noexcept {
        return {rows,
                header.count,
                collections,
                keys,
                {header.fields_begin, sizeof(std::string_view)},
                {header.types_begin, sizeof(ustore_docs_field_type_t)},
                columns};
    }
};

#pragma endregion Tables

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
deferrable<answer_type_, completion_> docs_read_(receiver_t const& receiver, place_batch_t const& places,
                                                 ustore_strings_view_t fields, arena_t& arena,
                                                 ustore_docs_field_type_t type, ustore_watch_t watch) noexcept {
    return answered_<deferrable<answer_type_, completion_>>(
        [&](ustore_outcome_t* outcome, outputs<answer_type_>* written) noexcept {
            written->count = places.count;
            ustore_docs_read_t arguments {.database = receiver.database,
                                          .executor = receiver.executor,
                                          .outcome = outcome,
                                          .completion = completion_,
                                          .view = receiver.view(watch),
                                          .count = places.count,
                                          .collections = places.collections(),
                                          .keys = places.keys(),
                                          .fields = fields,
                                          .presences = &written->presences,
                                          .lengths = lengths_output_(*written),
                                          .values = values_output_(*written),
                                          .arena = arena.handle(),
                                          .outputs_type = type};
            ustore_docs_read(&arguments);
        });
}

/**
 *  @brief Writes @p values to the documents at @p places, or to the fields @p fields name inside
 *      them, as @p modification says.
 *  @param[in] values Values arriving as @p type, borrowed until the call is final.
 *  @param[in] durability Whether the call finishes only after the data reaches the disk.
 */
template <ustore_completion_t completion_>
deferrable<void, completion_> docs_write_(receiver_t const& receiver, place_batch_t const& places,
                                          ustore_strings_view_t fields, ustore_strings_view_t values,
                                          ustore_docs_field_type_t type, ustore_docs_modification_t modification,
                                          ustore_durability_t durability) noexcept {
    return answered_<deferrable<void, completion_>>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_docs_write_t arguments {.database = receiver.database,
                                       .executor = receiver.executor,
                                       .outcome = outcome,
                                       .completion = completion_,
                                       .transaction = receiver.transaction,
                                       .count = places.count,
                                       .collections = places.collections(),
                                       .keys = places.keys(),
                                       .fields = fields,
                                       .values = values,
                                       .type = type,
                                       .modification = modification,
                                       .durability = durability};
        ustore_docs_write(&arguments);
    });
}

/** Removes the documents at @p places whole, while a patch or a merge removes
 *  fields inside them. */
template <ustore_completion_t completion_>
deferrable<void, completion_> docs_remove_(receiver_t const& receiver, place_batch_t const& places,
                                           ustore_durability_t durability) noexcept {
    return answered_<deferrable<void, completion_>>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_docs_remove_t arguments {.database = receiver.database,
                                        .executor = receiver.executor,
                                        .outcome = outcome,
                                        .completion = completion_,
                                        .transaction = receiver.transaction,
                                        .count = places.count,
                                        .collections = places.collections(),
                                        .keys = places.keys(),
                                        .durability = durability};
        ustore_docs_remove(&arguments);
    });
}

/** Lists the unique paths to the leaves of the documents at @p places into @p arena. */
template <ustore_completion_t completion_>
deferrable<strings_view_t, completion_> docs_gist_(receiver_t const& receiver, place_batch_t const& places,
                                                   arena_t& arena, ustore_watch_t watch) noexcept {
    return answered_<deferrable<strings_view_t, completion_>>(
        [&](ustore_outcome_t* outcome, outputs<strings_view_t>* written) noexcept {
            ustore_docs_gist_t arguments {.database = receiver.database,
                                          .executor = receiver.executor,
                                          .outcome = outcome,
                                          .completion = completion_,
                                          .view = receiver.view(watch),
                                          .count = places.count,
                                          .collections = places.collections(),
                                          .keys = places.keys(),
                                          .fields = &written->strings,
                                          .written = &written->written,
                                          .arena = arena.handle()};
            ustore_docs_gist(&arguments);
        });
}

/** Gathers every column of @p header from the documents at @p places into @p arena, as a table
 *  whose index and header view the places and the header, both borrowed until the call is final and
 *  read by the table after. */
template <ustore_completion_t completion_, typename table_type_>
deferrable<table_type_, completion_> docs_gather_(receiver_t const& receiver, place_batch_t const& places,
                                                  table_header_view_t const& header, arena_t& arena,
                                                  ustore_watch_t watch) noexcept {
    return answered_<deferrable<table_type_, completion_>>(
        [&](ustore_outcome_t* outcome, outputs<table_type_>* written) noexcept {
            written->rows = places.count;
            written->collections = places.collections_begin;
            written->keys = places.keys_begin;
            written->header = header;
            ustore_docs_gather_t arguments {
                .database = receiver.database,
                .executor = receiver.executor,
                .outcome = outcome,
                .completion = completion_,
                .view = receiver.view(watch),
                .count = places.count,
                .collections = places.collections(),
                .keys = places.keys(),
                .fields_count = header.count,
                .fields = strings_view_of(std::span<std::string_view const> {header.fields_begin, header.count}),
                .columns = &written->columns,
                .arena = arena.handle(),
                .outputs_types = header.types_begin};
            ustore_docs_gather(&arguments);
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
    status_t write_(values_argument_type_&& values, ustore_docs_modification_t modification,
                    ustore_docs_field_type_t type, ustore_durability_t durability) noexcept {
        values_arg_extractor<std::decay_t<values_argument_type_>> extractor;
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
    expected<strings_view_t> gist(arena_t& arena, ustore_watch_t watch = ustore_watch_keys_k) noexcept {
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

#pragma region Collections

/** A walk over the keys of one documents collection, whose pairs are whole documents. */
using docs_cursor_t = keys_cursor<ustore_docs_cursor_t, ustore_docs_cursor_init_t, ustore_docs_cursor_next_t,
                                  &ustore_docs_cursor_init, &ustore_docs_cursor_next, &ustore_docs_cursor_free>;

/** The fields a call over @p count keys passes as @p fields: none for whole documents, one for
 *  every key, which an accessor whose handle is @p fields repeats, or one per key. */
template <strings_batch fields_type_>
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
 *  A blocking handle answers a @c status_t or an @c expected, while a deferred one answers a
 *  @c deferred call, borrowing the handle, its inputs and its arena until the call is final. A
 *  handle with @c collection_access_t::read_only_k only reads, as a snapshot hands out. The type of
 *  a handle shapes only how values cross it, since every document is stored as JSON.
 */
template <ustore_completion_t completion_, collection_access_t access_>
class docs_collection_on {
    template <typename value_type_ = void>
    using deferrable = ustore::deferrable<value_type_, completion_>;
    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {};
    ustore_docs_field_type_t type_ {ustore_docs_field_type_json_k};
    static constexpr bool writes_k = access_ == collection_access_t::read_write_k;

    /** A proxy over one location inside this collection, read and written as @p type. */
    template <typename key_argument_type_>
        requires(!batch<key_argument_type_>)
    docs_ref<collection_key_field_t, access_> address_(key_argument_type_ const& key,
                                                       ustore_docs_field_type_t type) const noexcept {
        return {receiver_, location_in(collection_, key), type};
    }

    /** A proxy over a batch of locations inside this collection, read and written as @p type. */
    template <batch keys_argument_type_>
    docs_ref<locations_in_collection<keys_argument_type_>, access_> address_(
        keys_argument_type_&& keys, ustore_docs_field_type_t type) const noexcept {
        using locations_t = locations_in_collection<keys_argument_type_>;
        return {receiver_, locations_t {std::forward<keys_argument_type_>(keys), collection_}, type};
    }

    /** The proxy @c at answers: over a batch of locations, or over one. */
    template <typename keys_argument_type_>
    using ref_of =
        std::conditional_t<batch<keys_argument_type_>, docs_ref<locations_in_collection<keys_argument_type_>, access_>,
                           docs_ref<collection_key_field_t, access_>>;

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

    ustore_collection_id_t handle() const noexcept { return collection_; }
    receiver_t receiver() const noexcept { return receiver_; }

    /** The same collection, whose verbs block on @p executor, or on the calling
     *  thread without one. */
    completed_as<ustore_completion_blocking_k> on(executor_t const& executor) const noexcept {
        return {receiver_.on(executor.handle()), collection_, type_};
    }

    /** The same collection, whose verbs submit to @p executor and answer deferred calls. */
    completed_as<ustore_completion_deferred_k> deferred(executor_t const& executor) const noexcept {
        return {receiver_.on(executor.handle()), collection_, type_};
    }

#pragma region Verbs

    /**
     *  @brief Reads the documents of @p keys, or the fields @p fields name inside them,
     *      into @p arena.
     *  @param[in] keys Keys to read, borrowed until the call is final.
     *  @param[in] arena Memory the values land in, borrowed until the call is final, which
     *      the answer views.
     *  @param[in] fields None for whole documents, one field for every key, or one per key,
     *      borrowed until final.
     *  @param[in] type Format the values are exported in.
     *  @param[in] watch Whether a read inside a transaction records the keys for conflicts.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k The @p fields are neither none, one, nor one per key.
     */
    template <strings_batch fields_type_ = strings_view_t>
    deferrable<values_view_t> read(keys_view_t keys, arena_t& arena, fields_type_ const& fields = {},
                                   ustore_docs_field_type_t type = ustore_docs_field_type_json_k,
                                   ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        ustore_strings_view_t fields_view {};
        if (status_t const pointed = fields_view_(fields, keys.size(), fields_view); pointed.failed())
            return answered_<deferrable<values_view_t>>(pointed);
        return docs_read_<completion_, values_view_t>(receiver_, places_in(collection_, keys), fields_view, arena, type,
                                                      watch);
    }

    /**
     *  @brief Writes @p values to the documents of @p keys pairwise, or to the fields @p fields
     *      name inside them.
     *  @param[in] keys Keys to write, borrowed until the call is final.
     *  @param[in] values One value per key, borrowed until the call is final.
     *  @param[in] fields None for whole documents, one field for every key, or one per key,
     *      borrowed until final.
     *  @param[in] modification Whether the values upsert, update or insert, or patch or merge
     *      what they find.
     *  @param[in] type Format the values arrive in.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k The @p keys and @p values differ in count, or the
     *    @p fields are neither none, one, nor one per key.
     */
    template <strings_batch values_type_, strings_batch fields_type_ = strings_view_t>
    deferrable<> write(keys_view_t keys, values_type_ const& values, fields_type_ const& fields = {},
                       ustore_docs_modification_t modification = ustore_docs_modify_upsert_k,
                       ustore_docs_field_type_t type = ustore_docs_field_type_json_k,
                       ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        // The call has one count, so only this side sees the keys and the values disagree
        if (keys.size() != std::size(values))
            return answered_<deferrable<>>({status_code_t::invalid_argument_k, "Every key needs its own value"});
        ustore_strings_view_t fields_view {};
        if (status_t const pointed = fields_view_(fields, keys.size(), fields_view); pointed.failed())
            return answered_<deferrable<>>(pointed);
        return docs_write_<completion_>(receiver_, places_in(collection_, keys), fields_view, strings_view_of(values),
                                        type, modification, durability);
    }

    /**
     *  @brief Removes the documents of @p keys whole, while a patch or a merge removes
     *      fields inside them.
     *  @param[in] keys Keys to remove, borrowed until the call is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     */
    deferrable<> remove(keys_view_t keys, ustore_durability_t durability = ustore_durability_buffered_k) const noexcept
        requires writes_k
    {
        return docs_remove_<completion_>(receiver_, places_in(collection_, keys), durability);
    }

    /**
     *  @brief Lists the unique paths to the leaves of the documents of @p keys into @p arena.
     *  @param[in] keys Keys of the documents, borrowed until the call is final.
     *  @param[in] arena Memory the paths land in, borrowed until the call is final, which
     *      the answer views.
     *  @param[in] watch Whether a read inside a transaction records the keys for conflicts.
     */
    deferrable<strings_view_t> gist(keys_view_t keys, arena_t& arena,
                                    ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return docs_gist_<completion_>(receiver_, places_in(collection_, keys), arena, watch);
    }

    /**
     *  @brief Gathers the columns @p header names from the documents of @p keys into @p arena, as
     *      a table typed at runtime, whose index views @p keys and this handle and whose header
     *      views @p header.
     *  @param[in] keys Keys of the documents, one per row, borrowed until the call is final and
     *      read by the table.
     *  @param[in] header Field and type of every column, borrowed until the call is final and read
     *      by the table.
     *  @param[in] arena Memory the columns land in, borrowed until the call is final, which
     *      the table views.
     *  @param[in] watch Whether a read inside a transaction records the keys for conflicts.
     */
    deferrable<docs_table_t> gather(keys_view_t keys, table_header_view_t const& header, arena_t& arena,
                                    ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return docs_gather_<completion_, docs_table_t>(receiver_, places_in(collection_, keys), header, arena, watch);
    }

    /** Gathers the columns @p header names from the documents of @p keys into @p arena, as a table
     *  typed at compile time, borrowing what the runtime-typed form borrows. */
    template <typename... column_types_>
    deferrable<docs_table<column_types_...>> gather(keys_view_t keys, table_header<column_types_...> const& header,
                                                    arena_t& arena,
                                                    ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return docs_gather_<completion_, docs_table<column_types_...>>(receiver_, places_in(collection_, keys),
                                                                       header.view(), arena, watch);
    }

    /**
     *  @brief Draws up to @p wanted document keys at random into @p arena, exporting no documents,
     *      so a read or a gather follows the sample.
     *  @param[in] arena Memory the keys land in, which the answer views, borrowed until the
     *      call is final.
     *  @param[in] watch Whether a read inside a transaction records the keys for conflicts.
     */
    deferrable<std::span<ustore_key_t const>> sample(std::size_t wanted, arena_t& arena,
                                                     ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return sample_keys_<completion_, ustore_docs_sample_t, &ustore_docs_sample>(receiver_, collection_, wanted,
                                                                                    arena, watch);
    }

    /**
     *  @brief Estimates the documents and the space from @p start_key, inclusive, to @p end_key,
     *      exclusive, into @p arena.
     *  @param[in] start_key First key of the range, borrowed until the call is final.
     *  @param[in] end_key Key the range stops before, borrowed until the call is final.
     *  @param[in] arena Memory the bounds land in, borrowed until the call is final.
     *  @param[in] watch Whether a read inside a transaction records the range for conflicts.
     */
    deferrable<size_estimates_t> measure(ustore_key_t const& start_key, ustore_key_t const& end_key, arena_t& arena,
                                         ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return measure_keys_<completion_, ustore_docs_measure_t, &ustore_docs_measure>(
            receiver_, collection_, start_key, end_key, arena, watch);
    }

    /** Empties every document, keeping the keys. */
    deferrable<> clear_values() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_values_k);
    }

    /** Removes every document, keeping the collection. */
    deferrable<> clear() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_pairs_k);
    }

    /** Removes the collection with all its documents. */
    deferrable<> drop() const noexcept
        requires writes_k
    {
        return collection_drop_<completion_>(receiver_, collection_, ustore_drop_collection_k);
    }

#pragma endregion Verbs

#pragma region Shorthand

    blobs_range_t members(ustore_key_t start_key = smallest_key_k, ustore_key_t end_key = largest_key_k) const noexcept
        requires blocks<completion_>
    {
        return {receiver_, collection_, start_key, end_key, modality_k};
    }

    keys_range_t keys(ustore_key_t start_key = smallest_key_k, ustore_key_t end_key = largest_key_k) const noexcept
        requires blocks<completion_>
    {
        return members(start_key, end_key).keys();
    }

    /** The bounds on the documents the engine estimates over the whole key range, through a
     *  one-off arena. */
    expected<size_range_t> size_range() const noexcept
        requires blocks<completion_>
    {
        return measured_size_range_<ustore_docs_measure_t, &ustore_docs_measure>(receiver_, collection_);
    }

    /** Counts the documents exactly by streaming through every key. */
    expected<std::size_t> size() const noexcept
        requires blocks<completion_>
    {
        return keys().size();
    }

    docs_ref<place_batch_t, access_> operator[](std::initializer_list<ustore_key_t> keys) const noexcept
        requires blocks<completion_>
    {
        return at(keys);
    }

    docs_ref<place_batch_t, access_> at(std::initializer_list<ustore_key_t> keys) const noexcept
        requires blocks<completion_>
    {
        return at(keys_view_t {strided(keys)});
    }

    docs_ref<place_batch_t, access_> operator[](keys_view_t keys) const noexcept
        requires blocks<completion_>
    {
        return at(keys);
    }

    docs_ref<place_batch_t, access_> at(keys_view_t keys) const noexcept
        requires blocks<completion_>
    {
        return at(keys, type_);
    }

    docs_ref<place_batch_t, access_> at(keys_view_t keys, ustore_docs_field_type_t type) const noexcept
        requires blocks<completion_>
    {
        return {receiver_, places_in(collection_, keys), type};
    }

    template <typename keys_argument_type_>
    ref_of<keys_argument_type_> operator[](keys_argument_type_&& keys) const noexcept
        requires blocks<completion_>
    {
        return at(std::forward<keys_argument_type_>(keys));
    }

    template <typename keys_argument_type_>
    ref_of<keys_argument_type_> at(keys_argument_type_&& keys) const noexcept
        requires blocks<completion_>
    {
        return at(std::forward<keys_argument_type_>(keys), type_);
    }

    /** Addresses one location or a batch of them inside this collection, read and written as @p
     *  type. */
    template <typename keys_argument_type_>
    ref_of<keys_argument_type_> at(keys_argument_type_&& keys, ustore_docs_field_type_t type) const noexcept
        requires blocks<completion_>
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

#pragma endregion Collections

} // namespace unum::ustore
