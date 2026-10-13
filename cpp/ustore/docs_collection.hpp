/**
 *  @file cpp/ustore/docs_collection.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief Handles to a collection of JSON documents, and their deferred twins, whose verbs submit to an executor.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <concepts>         // `std::same_as`
#include <initializer_list> // `std::initializer_list`
#include <type_traits>      // `std::remove_cvref_t`
#include <utility>          // `std::forward`, `std::move`

#include "ustore/blobs_collection.hpp" // `blobs_read_t`, `blobs_remove_t`
#include "ustore/blobs_range.hpp"      // `blobs_range_t`, `keys_range_t`, `size_range_t`
#include "ustore/docs.h"
#include "ustore/docs_ref.hpp"   // `docs_read_ref`, `docs_ref`
#include "ustore/docs_table.hpp" // `docs_table_t`, `field_type_t`
#include "ustore/ranges.hpp"     // `place_batch_t`, `owned_array_concept`, `values_tape_t`, `tape_of`, `strings_of`
#include "ustore/status.hpp"     // `status_t`, `expected`, `deferred`
#include "ustore/types.hpp"      // `arena_t`, `receiver_t`, `collection_key_t`, `has_field_concept`

namespace unum::ustore {

class deferred_docs_read_collection_t;
class deferred_docs_collection_t;

#pragma region Collections

/**
 *  @brief Map from keys to JSON documents that only reads, cheap to construct and to copy.
 *
 *  A handle addresses either the current state of a collection or its view inside a transaction or a snapshot,
 *  holds no state of its own, so any number of threads share it, and never outlives its database.
 *  The type of a handle shapes only how values cross it, since documents are stored as JSON whatever they arrive as.
 */
class docs_read_collection_t {
  protected:
    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {ustore_collection_main_k};
    ustore_docs_field_type_t type_ {ustore_docs_field_type_json_k};

    /** A @p reference_ proxy over one location or a batch of them inside this collection, read and written as
     *  @p type. */
    template <template <typename> typename reference_, typename keys_argument_type_>
    auto address_(keys_argument_type_&& keys, ustore_docs_field_type_t type) noexcept {
        if constexpr (!batch_concept<keys_argument_type_>) {
            using plain_t = std::remove_cvref_t<keys_argument_type_>;
            collection_key_field_t location;
            location.collection = collection_;
            if constexpr (std::same_as<plain_t, ustore_key_t>) location.key = keys;
            else location.key = keys.key;

            if constexpr (has_field_concept<plain_t>) location.field = keys.field;
            return reference_<collection_key_field_t> {receiver_, std::move(location), type};
        }
        else {
            using locations_t = locations_in_collection<keys_argument_type_>;
            return reference_<locations_t> {receiver_,
                                            locations_t {std::forward<keys_argument_type_>(keys), collection_}, type};
        }
    }

    /** A @p reference_ proxy over a strided batch of keys inside this collection, read and written as @p type. */
    template <template <typename> typename reference_>
    reference_<place_batch_t> address_keys_(keys_view_t keys, ustore_docs_field_type_t type) noexcept {
        place_batch_t places;
        places.collections_begin = &collection_;
        places.keys_begin = keys.begin();
        places.count = keys.size();
        return {receiver_, std::move(places), type};
    }

  public:

    /** The handle a snapshot hands out for this kind of collection, which only reads. */
    using read_only_t = docs_read_collection_t;

    /** The twin a deferred receiver hands out, whose verbs answer deferred calls. */
    using deferred_t = deferred_docs_read_collection_t;

    /** What the collections this kind of handle addresses hold, which a creation records and a lookup checks. */
    static constexpr ustore_modality_t modality_k = ustore_modality_docs_k;

    docs_read_collection_t() noexcept = default;

    docs_read_collection_t(receiver_t receiver, ustore_collection_id_t collection = ustore_collection_main_k,
                           ustore_docs_field_type_t type = ustore_docs_field_type_json_k) noexcept
        : receiver_(receiver), collection_(collection), type_(type) {}

    operator ustore_collection_id_t() const noexcept { return collection_; }
    ustore_collection_id_t* member_ptr() noexcept { return &collection_; }

    blobs_range_t members( //
        ustore_key_t start_key = smallest_key_k, ustore_key_t end_key = largest_key_k) const noexcept {
        return {receiver_, collection_, start_key, end_key};
    }

    keys_range_t keys( //
        ustore_key_t start_key = smallest_key_k, ustore_key_t end_key = largest_key_k) const noexcept {
        return {members(start_key, end_key)};
    }

    expected<size_range_t> size_range() const noexcept {
        auto [estimates, size_range_status] = members().size_estimates();
        if (size_range_status.failed()) return size_range_status;
        return estimates.cardinality;
    }

    /** Estimates the number of documents as the middle of @c size_range. */
    expected<std::size_t> size() const noexcept {
        auto [range, size_range_status] = size_range();
        if (size_range_status.failed()) return size_range_status;
        return (range.min + range.max) / 2;
    }

    docs_read_ref<place_batch_t> operator[](std::initializer_list<ustore_key_t> keys) noexcept { return at(keys); }
    docs_read_ref<place_batch_t> at(std::initializer_list<ustore_key_t> keys) noexcept { return at(strided(keys)); }

    docs_read_ref<place_batch_t> operator[](keys_view_t keys) noexcept { return at(keys); }
    docs_read_ref<place_batch_t> at(keys_view_t keys) noexcept { return at(keys, type_); }

    docs_read_ref<place_batch_t> at(keys_view_t keys, ustore_docs_field_type_t type) noexcept {
        return address_keys_<docs_read_ref>(keys, type);
    }

    template <typename keys_argument_type_>
    auto operator[](keys_argument_type_&& keys) noexcept {
        return at(std::forward<keys_argument_type_>(keys));
    }

    template <typename keys_argument_type_>
    auto at(keys_argument_type_&& keys) noexcept {
        return at(std::forward<keys_argument_type_>(keys), type_);
    }

    template <typename keys_argument_type_>
    auto at(keys_argument_type_&& keys, ustore_docs_field_type_t type) noexcept {
        return address_<docs_read_ref>(std::forward<keys_argument_type_>(keys), type);
    }
};

/** Transactional map from keys to JSON documents, cheap to construct, which reads and writes the head or the view of
 *  a transaction. */
class docs_collection_t : public docs_read_collection_t {

    status_t drop_with_(ustore_drop_mode_t mode) noexcept {
        ustore_outcome_t outcome {};
        ustore_collection_drop_t collection_drop {};
        collection_drop.database = receiver_.database;
        collection_drop.executor = receiver_.executor;
        collection_drop.outcome = &outcome;
        collection_drop.collection = collection_;
        collection_drop.mode = mode;
        ustore_collection_drop(&collection_drop);
        return status_t {outcome};
    }

  public:
    using docs_read_collection_t::docs_read_collection_t;

    /** The twin a deferred receiver hands out, whose verbs answer deferred calls. */
    using deferred_t = deferred_docs_collection_t;

    /** Empties every document, keeping the keys. */
    status_t clear_values() noexcept { return drop_with_(ustore_drop_values_k); }

    /** Removes every document, keeping the collection. */
    status_t clear() noexcept { return drop_with_(ustore_drop_pairs_k); }

    /** Removes the collection with all its documents. */
    status_t drop() noexcept { return drop_with_(ustore_drop_collection_k); }

    docs_ref<place_batch_t> operator[](std::initializer_list<ustore_key_t> keys) noexcept { return at(keys); }
    docs_ref<place_batch_t> at(std::initializer_list<ustore_key_t> keys) noexcept { return at(strided(keys)); }

    docs_ref<place_batch_t> operator[](keys_view_t keys) noexcept { return at(keys); }
    docs_ref<place_batch_t> at(keys_view_t keys) noexcept { return at(keys, type_); }

    docs_ref<place_batch_t> at(keys_view_t keys, ustore_docs_field_type_t type) noexcept {
        return address_keys_<docs_ref>(keys, type);
    }

    template <typename keys_argument_type_>
    auto operator[](keys_argument_type_&& keys) noexcept {
        return at(std::forward<keys_argument_type_>(keys));
    }

    template <typename keys_argument_type_>
    auto at(keys_argument_type_&& keys) noexcept {
        return at(std::forward<keys_argument_type_>(keys), type_);
    }

    template <typename keys_argument_type_>
    auto at(keys_argument_type_&& keys, ustore_docs_field_type_t type) noexcept {
        return address_<docs_ref>(std::forward<keys_argument_type_>(keys), type);
    }
};

#pragma endregion Collections

#pragma region Deferred Verbs

/** The fields a deferred call over @p count keys passes as @p fields: none for whole documents, one for every key,
 *  which @p repeated lends through an accessor over @p field, or one per key, which @p tape passes as it is. */
inline status_t fields_of_(values_tape_t const& tape, std::size_t count, value_view_t& field,
                           strided_strings<value_view_t>& repeated, ustore_strings_view_t& fields) noexcept {
    if (tape.size() > 1 && tape.size() != count)
        return status_t {status_code_t::invalid_argument_k, "Every key needs its own field, or one field serves all"};
    if (tape.size() != 1) {
        fields = tape_of(tape);
        return {};
    }
    field = value_view_t {std::string_view {tape[0].data(), tape[0].size()}};
    repeated = strided_strings<value_view_t> {{&field, 0}};
    fields = repeated.view();
    return {};
}

/** What a deferred @c ustore_docs_read keeps beside its keys and fields: the collection and the field every key
 *  shares, and the outputs the call writes into the arena it borrows. */
struct docs_read_t {
    using arguments_t = ustore_docs_read_t;
    using answer_t = expected<values_view_t>;

    /** Collection every key lives in. */
    ustore_collection_id_t collection {};

    /** The field every key narrows to, when one serves them all, and the accessor lending it. */
    value_view_t field {};
    strided_strings<value_view_t> repeated {};

    /** Number of keys, the length of every output. */
    ustore_size_t count = 0;

    /** Outputs for the presences and the tape of values, inside the arena. */
    ustore_octet_t const* presences = nullptr;
    ustore_strings_view_t values {};

    static void submit(arguments_t& arguments) noexcept { ustore_docs_read(&arguments); }
    answer_t finish() && noexcept { return values_view_t {count, presences, values}; }
};

/** What a deferred @c ustore_docs_write keeps beside its keys, values and fields: the collection and the field every
 *  key shares. */
struct docs_write_t {
    using arguments_t = ustore_docs_write_t;
    using answer_t = status_t;

    /** Collection every key lives in. */
    ustore_collection_id_t collection {};

    /** The field every key narrows to, when one serves them all, and the accessor lending it. */
    value_view_t field {};
    strided_strings<value_view_t> repeated {};

    static void submit(arguments_t& arguments) noexcept { ustore_docs_write(&arguments); }
    answer_t finish() && noexcept { return {}; }
};

/** What a deferred @c ustore_docs_gist keeps beside its keys: the collection, and the outputs the call writes into
 *  the arena it borrows. */
struct docs_gist_t {
    using arguments_t = ustore_docs_gist_t;
    using answer_t = expected<sz::strings_view_t>;

    /** Collection every key lives in. */
    ustore_collection_id_t collection {};

    /** Outputs for the number of paths and their tape, the latter inside the arena. */
    ustore_size_t paths_count = 0;
    ustore_strings_view_t paths {};

    static void submit(arguments_t& arguments) noexcept { ustore_docs_gist(&arguments); }
    answer_t finish() && noexcept { return strings_of(paths, paths_count); }
};

/** What a deferred @c ustore_docs_gather keeps: the rows, columns and field names its table views, which the answer
 *  takes over, and the columns the call writes into the arena it borrows. */
struct docs_gather_t {

    /** What a finished gather hands over: the table, and everything it views outside the arena. */
    struct finished_t {

        /** Names of the columns, which @c columns points into. */
        values_tape_t fields;

        /** The collection and key of every row, which the index of @c table views. */
        st::basic_vector<collection_key_t> rows;

        /** The field and type of every column, which the header of @c table views. */
        st::basic_vector<field_type_t> columns;

        /** Documents as rows and fields as columns, whose cells live in the arena. */
        docs_table_t table;
    };

    using arguments_t = ustore_docs_gather_t;
    using answer_t = expected<finished_t>;

    /** What the answer takes over once the call finishes, its table still empty. */
    finished_t outputs;

    /** The type of every column back to back, as the arguments read them. */
    st::basic_vector<ustore_docs_field_type_t> types;

    /** Output for one column per field, inside the arena. */
    ustore_docs_column_view_t const* columns = nullptr;

    static void submit(arguments_t& arguments) noexcept { ustore_docs_gather(&arguments); }

    answer_t finish() && noexcept {
        auto const rows = strided(outputs.rows.data(), outputs.rows.data() + outputs.rows.size()).immutable();
        auto const header =
            strided(outputs.columns.data(), outputs.columns.data() + outputs.columns.size()).immutable();
        outputs.table = docs_table_t {rows.size(),
                                      header.size(),
                                      rows.members(&collection_key_t::collection).begin(),
                                      rows.members(&collection_key_t::key).begin(),
                                      header.members(&field_type_t::field).begin(),
                                      header.members(&field_type_t::type).begin(),
                                      columns};
        return std::move(outputs);
    }
};

#pragma endregion Deferred Verbs

#pragma region Deferred Collections

/** A collection of JSON documents that only reads, whose verbs submit to an executor and answer deferred calls, as a
 *  snapshot offers it. Every plain input a verb reads moves into the call, so nothing it borrowed can go before the
 *  call is final, while an arena is borrowed, since freeing it waits for the call. */
class deferred_docs_read_collection_t {
  protected:
    receiver_t receiver_ {};
    ustore_collection_id_t collection_ {ustore_collection_main_k};

  public:
    deferred_docs_read_collection_t() noexcept = default;
    explicit deferred_docs_read_collection_t(receiver_t receiver,
                                             ustore_collection_id_t collection = ustore_collection_main_k) noexcept
        : receiver_(receiver), collection_(collection) {}

    operator ustore_collection_id_t() const noexcept { return collection_; }

    /**
     *  @brief Reads the documents of @p keys, or the fields @p fields name inside them, into @p arena.
     *  @param[in] keys Keys to read, owned by the call until it is final.
     *  @param[in] arena Memory the values land in, borrowed until the call is final, which the answer views.
     *  @param[in] fields None for whole documents, one field for every key, or one per key.
     *  @param[in] type Format the values are exported in.
     *  @param[in] watch Whether a read inside a transaction records the keys for conflicts.
     */
    template <owned_array_concept<ustore_key_t> keys_type_>
    deferred<docs_read_t> read(keys_type_ keys, arena_t& arena, values_tape_t fields = {},
                               ustore_docs_field_type_t type = ustore_docs_field_type_json_k,
                               ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return deferred<docs_read_t>::submit(
            [&](docs_read_t& verb, ustore_docs_read_t& arguments, keys_type_ const& owned_keys,
                values_tape_t const& owned_fields) noexcept {
                std::size_t const count = owned_keys.size();
                if (status_t const pointed = fields_of_(owned_fields, count, verb.field, verb.repeated,
                                                        arguments.fields);
                    pointed.failed())
                    return pointed;
                verb.collection = collection_;
                verb.count = count;
                arguments.database = receiver_.database;
                arguments.executor = receiver_.executor;
                arguments.view = receiver_.view(watch);
                arguments.count = count;
                arguments.collections = {&verb.collection, 0};
                arguments.keys = {owned_keys.data(), sizeof(ustore_key_t)};
                arguments.presences = &verb.presences;
                arguments.values = &verb.values;
                arguments.arena = arena;
                arguments.outputs_type = type;
                return status_t {};
            },
            std::move(keys), std::move(fields));
    }

    /**
     *  @brief Lists the unique paths to the leaves of the documents of @p keys into @p arena.
     *  @param[in] keys Keys of the documents, owned by the call until it is final.
     *  @param[in] arena Memory the paths land in, borrowed until the call is final, which the answer views.
     *  @param[in] watch Whether a read inside a transaction records the keys for conflicts.
     */
    template <owned_array_concept<ustore_key_t> keys_type_>
    deferred<docs_gist_t> gist(keys_type_ keys, arena_t& arena,
                               ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return deferred<docs_gist_t>::submit(
            [&](docs_gist_t& verb, ustore_docs_gist_t& arguments, keys_type_ const& owned) noexcept {
                verb.collection = collection_;
                arguments.database = receiver_.database;
                arguments.executor = receiver_.executor;
                arguments.view = receiver_.view(watch);
                arguments.count = owned.size();
                arguments.collections = {&verb.collection, 0};
                arguments.keys = {owned.data(), sizeof(ustore_key_t)};
                arguments.fields = &verb.paths;
                arguments.written = &verb.paths_count;
                arguments.arena = arena;
                return status_t {};
            },
            std::move(keys));
    }

    /**
     *  @brief Gathers the fields @p fields name from the documents of @p keys into @p arena, as a table typed at
     *      runtime.
     *  @param[in] keys Keys of the documents, one per row.
     *  @param[in] fields Fields to gather, one per column, which the finished gather hands back.
     *  @param[in] types Type every column converts its field into, one per field.
     *  @param[in] arena Memory the columns land in, borrowed until the call is final, which the table views.
     *  @param[in] watch Whether a read inside a transaction records the keys for conflicts.
     */
    template <owned_array_concept<ustore_key_t> keys_type_, owned_array_concept<ustore_docs_field_type_t> types_type_>
    deferred<docs_gather_t> gather(keys_type_ keys, values_tape_t fields, types_type_ types, arena_t& arena,
                                   ustore_watch_t watch = ustore_watch_keys_k) const noexcept {
        return deferred<docs_gather_t>::submit([&](docs_gather_t& verb, ustore_docs_gather_t& arguments) noexcept {
            docs_gather_t::finished_t& gathered = verb.outputs;
            if (types.size() != fields.size())
                return status_t {status_code_t::invalid_argument_k, "Every field of a gather needs its own type"};
            if (st::failed(gathered.rows.resize(keys.size())) || st::failed(gathered.columns.resize(types.size())) ||
                st::failed(verb.types.resize(types.size())))
                return status_t {status_code_t::out_of_memory_k, "Failed to allocate the rows and columns of a gather"};

            // The table views its rows and columns long after the call, so they live on the heap the answer owns
            gathered.fields = std::move(fields);
            for (std::size_t row = 0; row != keys.size(); ++row) gathered.rows[row] = {collection_, keys[row]};
            ustore_strings_view_t const names = tape_of(gathered.fields);
            for (std::size_t column = 0; column != types.size(); ++column) {
                gathered.columns[column] = {string_of(tape_string(names, column)), types[column]};
                verb.types[column] = types[column];
            }
            auto const rows = strided(gathered.rows.data(), gathered.rows.data() + gathered.rows.size()).immutable();
            auto const collections = rows.members(&collection_key_t::collection);
            auto const row_keys = rows.members(&collection_key_t::key);

            arguments.database = receiver_.database;
            arguments.executor = receiver_.executor;
            arguments.view = receiver_.view(watch);
            arguments.count = rows.size();
            arguments.collections = {collections.data(), collections.stride()};
            arguments.keys = {row_keys.data(), row_keys.stride()};
            arguments.fields_count = types.size();
            arguments.fields = tape_of(gathered.fields);
            arguments.columns = &verb.columns;
            arguments.arena = arena;
            arguments.outputs_types = verb.types.data();
            return status_t {};
        });
    }
};

/** A collection of JSON documents whose verbs submit to an executor and answer deferred calls, reading and writing the
 *  head or a transaction. */
class deferred_docs_collection_t : public deferred_docs_read_collection_t {
  public:
    using deferred_docs_read_collection_t::deferred_docs_read_collection_t;

    /**
     *  @brief Writes @p values to the documents of @p keys pairwise, or to the fields @p fields name inside them.
     *  @param[in] keys Keys to write, owned by the call until it is final.
     *  @param[in] values One value per key, owned by the call until it is final.
     *  @param[in] fields None for whole documents, one field for every key, or one per key.
     *  @param[in] modification Whether the values upsert, update or insert, or patch or merge what they find.
     *  @param[in] type Format the values arrive in.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     */
    template <owned_array_concept<ustore_key_t> keys_type_>
    deferred<docs_write_t> write(keys_type_ keys, values_tape_t values, values_tape_t fields = {},
                                 ustore_docs_modification_t modification = ustore_docs_modify_upsert_k,
                                 ustore_docs_field_type_t type = ustore_docs_field_type_json_k,
                                 ustore_durability_t durability = ustore_durability_buffered_k) const noexcept {
        return deferred<docs_write_t>::submit(
            [&](docs_write_t& verb, ustore_docs_write_t& arguments, keys_type_ const& owned_keys,
                values_tape_t const& owned_values, values_tape_t const& owned_fields) noexcept {
                std::size_t const count = owned_keys.size();
                if (count != owned_values.size())
                    return status_t {status_code_t::invalid_argument_k, "Every key of a write needs its own value"};
                if (status_t const pointed = fields_of_(owned_fields, count, verb.field, verb.repeated,
                                                        arguments.fields);
                    pointed.failed())
                    return pointed;
                verb.collection = collection_;
                arguments.database = receiver_.database;
                arguments.executor = receiver_.executor;
                arguments.transaction = receiver_.transaction;
                arguments.count = count;
                arguments.collections = {&verb.collection, 0};
                arguments.keys = {owned_keys.data(), sizeof(ustore_key_t)};
                arguments.values = tape_of(owned_values);
                arguments.type = type;
                arguments.modification = modification;
                arguments.durability = durability;
                return status_t {};
            },
            std::move(keys), std::move(values), std::move(fields));
    }

    /**
     *  @brief Removes the documents of @p keys whole, while a patch or a merge removes fields inside them.
     *  @param[in] keys Keys to remove, owned by the call until it is final.
     *  @param[in] durability Whether the call finishes only after the data reaches the disk.
     */
    template <owned_array_concept<ustore_key_t> keys_type_>
    deferred<blobs_remove_t> remove(keys_type_ keys,
                                    ustore_durability_t durability = ustore_durability_buffered_k) const noexcept {
        return deferred_blobs_collection_t {receiver_, collection_}.remove(std::move(keys), durability);
    }
};

#pragma endregion Deferred Collections

} // namespace unum::ustore
