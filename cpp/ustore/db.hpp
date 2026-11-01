/**
 *  @file cpp/ustore/db.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief The database, transaction and snapshot handles of the C++ wrappers, and the receivers that run their calls
 *      on an executor, blocking or deferred.
 *
 *  A receiver chooses where a call runs and the verb stays: @c on gives the same collections and
 *  proxies, running blocking on an executor, while @c deferred gives collections whose verbs take
 *  owned inputs and answer a @c deferred call. Snapshots hand out read-only collections and
 *  proxies, so nothing can write through one.
 */

#pragma once

#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::exchange`, `std::forward`, `std::swap`

#include <stringzilla/stringzilla.hpp> // `sz::string_t`

#include "ustore/blobs_collection.hpp"   // `blobs_collection_t`, `deferred_blobs_collection_t`
#include "ustore/blobs_ref.hpp"          // `blobs_ref`, `blobs_read_ref`
#include "ustore/docs_collection.hpp"    // what `find` and `create` take as a template argument
#include "ustore/graph_collection.hpp"   // what `find` and `create` take as a template argument
#include "ustore/ranges.hpp"             // `strided_span`, `place_batch_t`
#include "ustore/status.hpp"             // `status_t`, `expected`, `deferred`
#include "ustore/strings_collection.hpp" // what `find` and `create` take as a template argument
#include "ustore/types.hpp"              // `arena_t`, `receiver_t`, `read_stamp_t`, `commit_stamp_t`
#include "ustore/ustore.h"

namespace unum::ustore {

using collections_list_t = std::span<ustore_collection_id_t const>;

#pragma region Lookups

/**
 *  @brief Reads the collection @p name was given at the head, or at the snapshot of @p receiver when it names one,
 *      through a one-off arena, since a name answers one identifier.
 *
 *  Names resolve in two phases, so this is the only place a name reaches the database, and every other
 *  call addresses its collection by the identifier this returns.
 *
 *  @param[in] receiver The receiver whose snapshot the name is read at, or the head when it names none.
 *  @param[in] name The collection's name.
 *  @param[out] modality What the collection holds, written when given.
 */
inline expected<ustore_collection_id_t> collection_named(receiver_t const& receiver, std::string_view name,
                                                         ustore_modality_t* modality = nullptr) noexcept {
    auto [arena, arena_status] = arena_t::make(receiver.executor);
    if (arena_status.failed()) return arena_status;

    strided_strings<std::string_view> const names {{&name, 0}};
    ustore_octet_t const* presences = nullptr;
    ustore_collection_id_t const* collections = nullptr;
    ustore_modality_t const* modalities = nullptr;
    ustore_outcome_t outcome {};
    ustore_collection_find_t find {};
    find.database = receiver.database;
    find.executor = receiver.executor;
    find.outcome = &outcome;
    find.snapshot = receiver.snapshot;
    find.count = 1;
    find.names = names.view();
    find.presences = &presences;
    find.collections = &collections;
    find.modalities = &modalities;
    find.arena = arena;
    ustore_collection_find(&find);
    if (status_t const status {outcome}; status.failed()) return status;
    if (!bits_view_t {presences}[0]) return status_t {status_code_t::not_found_k, "No such collection"};
    if (modality) *modality = modalities[0];
    return collections[0];
}

/**
 *  @brief Reads the collection @p name was given, as the overload above, refusing one that holds
 *      another modality than @p modality.
 *
 *  @par Statuses:
 *  - @c status_code_t::not_found_k No collection carries @p name.
 *  - @c status_code_t::invalid_argument_k The collection holds another modality than @p modality.
 */
inline expected<ustore_collection_id_t> collection_named(receiver_t const& receiver, std::string_view name,
                                                         ustore_modality_t modality) noexcept {
    ustore_modality_t held {};
    auto [found, find_status] = collection_named(receiver, name, &held);
    if (find_status.failed()) return find_status;
    if (held != modality) return status_t {status_code_t::invalid_argument_k, "The collection holds another modality"};
    return found;
}

/** Whether a collection named @p name exists at the head, or at the snapshot of @p receiver. */
inline expected<bool> collection_exists(receiver_t const& receiver, std::string_view name) noexcept {
    auto [found, lookup_status] = collection_named(receiver, name);
    if (lookup_status.succeeded()) return true;
    if (lookup_status == status_code_t::not_found_k) return false;
    return lookup_status;
}

/** Lists the collections at the head, or at the snapshot of @p receiver, with their identifiers in @p arena. */
inline expected<collections_list_t> list_collections(receiver_t const& receiver, arena_t& arena) noexcept {
    ustore_size_t written = 0;
    ustore_collection_id_t const* ids = nullptr;
    ustore_outcome_t outcome {};
    ustore_collection_list_t collection_list {};
    collection_list.database = receiver.database;
    collection_list.executor = receiver.executor;
    collection_list.outcome = &outcome;
    collection_list.snapshot = receiver.snapshot;
    collection_list.collections = &ids;
    collection_list.written = &written;
    collection_list.arena = arena;

    ustore_collection_list(&collection_list);
    if (status_t const status {outcome}; status.failed()) return status;
    return collections_list_t {ids, ids ? written : 0};
}

/** The places of a batch of keys, each beside the collection it lives in. */
inline place_batch_t places_of_(strided_span<collection_key_t const> collections_and_keys) noexcept {
    place_batch_t places;
    places.collections_begin = collections_and_keys.members(&collection_key_t::collection).begin();
    places.keys_begin = collections_and_keys.members(&collection_key_t::key).begin();
    places.count = collections_and_keys.size();
    return places;
}

/** The places of a batch of keys, each beside its collection and the document field it narrows to. */
inline place_batch_t places_of_(strided_span<collection_key_field_t const> collections_and_keys) noexcept {
    place_batch_t places;
    places.collections_begin = collections_and_keys.members(&collection_key_field_t::collection).begin();
    places.keys_begin = collections_and_keys.members(&collection_key_field_t::key).begin();
    places.fields_begin = collections_and_keys.members(&collection_key_field_t::field).begin();
    places.count = collections_and_keys.size();
    return places;
}

#pragma endregion Lookups

#pragma region Deferred Verbs

/** Copies @p text into @p copy, answering the bytes of the copy, which a deferred call reads until it is final. */
inline expected<ustore_bytes_view_t> own_bytes_(std::string_view text, sz::string_t& copy) noexcept {
    if (copy.assign(sz::string_view_t {text}) != sz::status_t::success_k)
        return status_t {status_code_t::out_of_memory_k, "Failed to copy a string into a deferred call"};
    return bytes_of(std::string_view {copy});
}

/** What a deferred @c ustore_transaction_init keeps to reset a transaction: the handle the call reuses. */
struct reset_t {
    using arguments_t = ustore_transaction_init_t;
    using answer_t = status_t;

    /** The transaction the call resets in place, which the arguments point to. */
    ustore_transaction_t transaction = nullptr;

    static void submit(arguments_t& arguments) noexcept { ustore_transaction_init(&arguments); }
    answer_t finish() && noexcept { return {}; }
};

/** What a deferred @c ustore_transaction_stage keeps: the stamp the call writes. */
struct stage_t {
    using arguments_t = ustore_transaction_stage_t;
    using answer_t = expected<read_stamp_t>;

    /** Output for the stamp the transaction reads at. */
    ustore_stamp_t read_stamp = 0;

    static void submit(arguments_t& arguments) noexcept { ustore_transaction_stage(&arguments); }
    answer_t finish() && noexcept { return read_stamp_t {read_stamp}; }
};

/** What a deferred @c ustore_transaction_commit keeps: the stamp the call writes. */
struct commit_t {
    using arguments_t = ustore_transaction_commit_t;
    using answer_t = expected<commit_stamp_t>;

    /** Output for the position of the commit in commit order. */
    ustore_stamp_t commit_stamp = 0;

    static void submit(arguments_t& arguments) noexcept { ustore_transaction_commit(&arguments); }
    answer_t finish() && noexcept { return commit_stamp_t {commit_stamp}; }
};

/** What a deferred @c ustore_transaction_rollback keeps, which is nothing. */
struct rollback_t {
    using arguments_t = ustore_transaction_rollback_t;
    using answer_t = status_t;

    static void submit(arguments_t& arguments) noexcept { ustore_transaction_rollback(&arguments); }
    answer_t finish() && noexcept { return {}; }
};

/** What a deferred @c ustore_snapshot_save keeps: a copy of the path the arguments point to. */
struct save_t {
    using arguments_t = ustore_snapshot_save_t;
    using answer_t = status_t;

    /** Directory that receives the saved database, copied into the call. */
    sz::string_t path;

    static void submit(arguments_t& arguments) noexcept { ustore_snapshot_save(&arguments); }
    answer_t finish() && noexcept { return {}; }
};

/** What a deferred @c ustore_collection_create keeps: a copy of the name the arguments point to, and the identifier
 *  the call writes. */
struct create_t {
    using arguments_t = ustore_collection_create_t;
    using answer_t = expected<ustore_collection_id_t>;

    /** Name of the collection, copied into the call, empty for a collection without one. */
    sz::string_t name;

    /** Output for the identifier of the collection. */
    ustore_collection_id_t collection {};

    static void submit(arguments_t& arguments) noexcept { ustore_collection_create(&arguments); }
    answer_t finish() && noexcept { return collection; }
};

/** What a deferred @c ustore_collection_drop keeps, which is nothing. */
struct drop_t {
    using arguments_t = ustore_collection_drop_t;
    using answer_t = status_t;

    static void submit(arguments_t& arguments) noexcept { ustore_collection_drop(&arguments); }
    answer_t finish() && noexcept { return {}; }
};

/** What a deferred @c ustore_collection_list keeps: the outputs the call writes into the arena it borrows. */
struct list_t {
    using arguments_t = ustore_collection_list_t;
    using answer_t = expected<collections_list_t>;

    /** Output for the identifiers of the collections, inside the arena. */
    ustore_collection_id_t const* collections = nullptr;

    /** Output for the collections written. */
    ustore_size_t written = 0;

    static void submit(arguments_t& arguments) noexcept { ustore_collection_list(&arguments); }
    answer_t finish() && noexcept { return collections_list_t {collections, collections ? written : 0}; }
};

/** What a deferred @c ustore_database_describe keeps: the description the call writes. */
struct describe_t {
    using arguments_t = ustore_database_describe_t;
    using answer_t = expected<ustore_database_description_t>;

    /** Output for the description. */
    ustore_database_description_t description {};

    static void submit(arguments_t& arguments) noexcept { ustore_database_describe(&arguments); }
    answer_t finish() && noexcept { return description; }
};

/** What a deferred @c ustore_database_clear keeps, which is nothing. */
struct clear_t {
    using arguments_t = ustore_database_clear_t;
    using answer_t = status_t;

    static void submit(arguments_t& arguments) noexcept { ustore_database_clear(&arguments); }
    answer_t finish() && noexcept { return {}; }
};

/** What a deferred @c ustore_database_compact keeps, which is nothing. */
struct compact_t {
    using arguments_t = ustore_database_compact_t;
    using answer_t = status_t;

    static void submit(arguments_t& arguments) noexcept { ustore_database_compact(&arguments); }
    answer_t finish() && noexcept { return {}; }
};

#pragma endregion Deferred Verbs

#pragma region Deferred Receivers

/** A transaction whose verbs submit to an executor and answer deferred calls, as @c transaction_ref_t::deferred hands
 *  it out. A transaction serves one call at a time, so a caller waits on one deferred call before submitting the
 *  next. */
class deferred_transaction_t {
    receiver_t receiver_ {};

  public:
    explicit deferred_transaction_t(receiver_t receiver) noexcept : receiver_(receiver) {}

    /** The collection named @p name, read and written through the transaction, as the deferred twin
     *  of the read-only twin of @p collection_type_. */
    template <typename collection_type_ = blobs_collection_t>
    expected<typename collection_type_::deferred_t> find(std::string_view name) const noexcept {
        auto [found, find_status] = collection_named(receiver_, name, collection_type_::modality_k);
        if (find_status.failed()) return find_status;
        return typename collection_type_::deferred_t {receiver_, found};
    }

    expected<deferred_blobs_collection_t> operator[](std::string_view name) const noexcept { return find(name); }

    /** Clears the transaction for reuse, keeping its memory and starting a new generation. */
    deferred<reset_t> reset() const noexcept {
        return deferred<reset_t>::submit([&](reset_t& verb, ustore_transaction_init_t& arguments) noexcept {
            verb.transaction = receiver_.transaction;
            arguments.transaction = &verb.transaction;
            arguments.database = receiver_.database;
            arguments.executor = receiver_.executor;
            return status_t {};
        });
    }

    /** Stages the transaction as the first phase of a two-phase commit, answering the stamp it reads at. */
    deferred<stage_t> stage() const noexcept {
        return deferred<stage_t>::submit([&](stage_t& verb, ustore_transaction_stage_t& arguments) noexcept {
            arguments.database = receiver_.database;
            arguments.executor = receiver_.executor;
            arguments.transaction = receiver_.transaction;
            arguments.read_stamp = &verb.read_stamp;
            return status_t {};
        });
    }

    /** Commits every update of the transaction, or none of them, answering the commit's stamp. */
    deferred<commit_t> commit(ustore_durability_t durability = ustore_durability_buffered_k) const noexcept {
        return deferred<commit_t>::submit([&](commit_t& verb, ustore_transaction_commit_t& arguments) noexcept {
            arguments.database = receiver_.database;
            arguments.executor = receiver_.executor;
            arguments.transaction = receiver_.transaction;
            arguments.commit_stamp = &verb.commit_stamp;
            arguments.durability = durability;
            return status_t {};
        });
    }

    /** Withdraws a stage, keeping the reads and writes. */
    deferred<rollback_t> rollback() const noexcept {
        return deferred<rollback_t>::submit([&](rollback_t&, ustore_transaction_rollback_t& arguments) noexcept {
            arguments.database = receiver_.database;
            arguments.executor = receiver_.executor;
            arguments.transaction = receiver_.transaction;
            return status_t {};
        });
    }
};

/** A snapshot whose verbs submit to an executor and answer deferred calls, reading only, as
 *  @c snapshot_ref_t::deferred hands it out. */
class deferred_snapshot_t {
    receiver_t receiver_ {};

  public:
    explicit deferred_snapshot_t(receiver_t receiver) noexcept : receiver_(receiver) {}

    /** The collection named @p name at the snapshot, as the deferred twin of the
     *  read-only twin of @p collection_type_. */
    template <typename collection_type_ = blobs_collection_t>
    expected<typename collection_type_::read_only_t::deferred_t> find(std::string_view name) const noexcept {
        auto [found, find_status] = collection_named(receiver_, name, collection_type_::modality_k);
        if (find_status.failed()) return find_status;
        return typename collection_type_::read_only_t::deferred_t {receiver_, found};
    }

    expected<deferred_blobs_read_collection_t> operator[](std::string_view name) const noexcept { return find(name); }

    /** Saves the snapshot as a new database at @p path, which the call copies, reporting the bytes saved to
     *  @p progress, borrowed until the call is final. */
    deferred<save_t> save(std::string_view path, progress_arg_t progress = {}) const noexcept {
        return deferred<save_t>::submit([&](save_t& verb, ustore_snapshot_save_t& arguments) noexcept {
            auto [copied, copy_status] = own_bytes_(path, verb.path);
            if (copy_status.failed()) return copy_status;
            arguments.snapshot = receiver_.snapshot;
            arguments.executor = receiver_.executor;
            arguments.path = copied;
            arguments.progress = progress.callback;
            arguments.progress_state = progress.state;
            return status_t {};
        });
    }
};

/** The head of a database, whose verbs submit to an executor and answer deferred calls, as
 *  @c database_ref_t::deferred hands it out. */
class deferred_database_t {
    receiver_t receiver_ {};

  public:
    explicit deferred_database_t(receiver_t receiver) noexcept : receiver_(receiver) {}

    /** The collection named @p name at the head, as the deferred twin of @p collection_type_. */
    template <typename collection_type_ = blobs_collection_t>
    expected<typename collection_type_::deferred_t> find(std::string_view name) const noexcept {
        auto [found, find_status] = collection_named(receiver_, name, collection_type_::modality_k);
        if (find_status.failed()) return find_status;
        return typename collection_type_::deferred_t {receiver_, found};
    }

    /** The binary collection @p collection identifies, resolved beforehand, so addressing it never blocks. */
    deferred_blobs_collection_t operator[](ustore_collection_id_t collection) const noexcept {
        return deferred_blobs_collection_t {receiver_, collection};
    }

    /** Creates a collection holding the modality of @p collection_type_ under @p name, which the
     *  call copies and no other collection may carry, answering its identifier. */
    template <typename collection_type_ = blobs_collection_t>
    deferred<create_t> create(std::string_view name) const noexcept {
        return deferred<create_t>::submit([&](create_t& verb, ustore_collection_create_t& arguments) noexcept {
            auto [copied, copy_status] = own_bytes_(name, verb.name);
            if (copy_status.failed()) return copy_status;
            arguments.database = receiver_.database;
            arguments.executor = receiver_.executor;
            if (!name.empty()) arguments.name = copied;
            arguments.modality = collection_type_::modality_k;
            arguments.collection = &verb.collection;
            return status_t {};
        });
    }

    /** Drops what @p mode names of the collection @p collection identifies. */
    deferred<drop_t> drop(ustore_collection_id_t collection,
                          ustore_drop_mode_t mode = ustore_drop_collection_k) const noexcept {
        return deferred<drop_t>::submit([&](drop_t&, ustore_collection_drop_t& arguments) noexcept {
            arguments.database = receiver_.database;
            arguments.executor = receiver_.executor;
            arguments.collection = collection;
            arguments.mode = mode;
            return status_t {};
        });
    }

    /** Lists the collections at the head, with their identifiers in @p arena, borrowed until the call is final. */
    deferred<list_t> collections(arena_t& arena) const noexcept {
        return deferred<list_t>::submit([&](list_t& verb, ustore_collection_list_t& arguments) noexcept {
            arguments.database = receiver_.database;
            arguments.executor = receiver_.executor;
            arguments.collections = &verb.collections;
            arguments.written = &verb.written;
            arguments.arena = arena;
            return status_t {};
        });
    }

    /** Reports the engine of the database, what it supports and how much it holds. */
    deferred<describe_t> describe() const noexcept {
        return deferred<describe_t>::submit([&](describe_t& verb, ustore_database_describe_t& arguments) noexcept {
            arguments.database = receiver_.database;
            arguments.executor = receiver_.executor;
            arguments.description = &verb.description;
            return status_t {};
        });
    }

    /** Empties every collection at once, as @p mode names. */
    deferred<clear_t> clear(ustore_drop_mode_t mode) const noexcept {
        return deferred<clear_t>::submit([&](clear_t&, ustore_database_clear_t& arguments) noexcept {
            arguments.database = receiver_.database;
            arguments.executor = receiver_.executor;
            arguments.mode = mode;
            return status_t {};
        });
    }

    /** Reclaims the space overwritten and removed data still takes, reporting its advance to @p progress, borrowed
     *  until the call is final. */
    deferred<compact_t> compact(progress_arg_t progress = {}) const noexcept {
        return deferred<compact_t>::submit([&](compact_t&, ustore_database_compact_t& arguments) noexcept {
            arguments.database = receiver_.database;
            arguments.executor = receiver_.executor;
            arguments.progress = progress.callback;
            arguments.progress_state = progress.state;
            return status_t {};
        });
    }
};

#pragma endregion Deferred Receivers

#pragma region Transactions

/** A transaction, as the calls on one executor see it, without owning either.
 *
 *  Batched writes are buffered in the layout the engine prefers until the commit, so a transaction also saves
 *  preprocessing. */
class transaction_ref_t {
  protected:
    receiver_t receiver_ {};

  public:
    transaction_ref_t() noexcept = default;
    explicit transaction_ref_t(receiver_t receiver) noexcept : receiver_(receiver) {}

    ustore_transaction_t transaction() const noexcept { return receiver_.transaction; }
    receiver_t receiver() const noexcept { return receiver_; }
    operator ustore_transaction_t() const noexcept { return receiver_.transaction; }

    /** The same transaction, running every call on @p executor. */
    transaction_ref_t on(ustore_executor_t executor) const noexcept {
        return transaction_ref_t {receiver_.on(executor)};
    }

    /** The same transaction, whose verbs submit to @p executor and answer deferred calls. */
    deferred_transaction_t deferred(ustore_executor_t executor) const noexcept {
        return deferred_transaction_t {receiver_.on(executor)};
    }

    blobs_ref<place_batch_t> operator[](strided_span<collection_key_t const> collections_and_keys) noexcept {
        return {receiver_, places_of_(collections_and_keys)};
    }

    blobs_ref<place_batch_t> operator[](strided_span<collection_key_field_t const> collections_and_keys) noexcept {
        return {receiver_, places_of_(collections_and_keys)};
    }

    template <typename keys_argument_type_>
        requires has_collection_concept<batch_element_of<keys_argument_type_>>
    blobs_ref<keys_argument_type_> operator[](keys_argument_type_&& keys) noexcept {
        return blobs_ref<keys_argument_type_> {receiver_, std::forward<keys_argument_type_>(keys)};
    }

    expected<blobs_collection_t> operator[](std::string_view name) noexcept { return find(name); }

    /** Whether a collection named @p name exists at the head. */
    expected<bool> contains(std::string_view name) noexcept { return collection_exists(receiver_, name); }

    /**
     *  @brief Opens a view of one collection that follows this transaction.
     *
     *  @tparam collection_type_ The collection type: @c blobs_collection_t, @c docs_collection_t,
     *      as well as @c graph_collection_t or @c strings_collection_t.
     */
    template <typename collection_type_ = blobs_collection_t>
    expected<collection_type_> find(std::string_view name) noexcept {
        auto [found, lookup_status] = collection_named(receiver_, name, collection_type_::modality_k);
        if (lookup_status.failed()) return lookup_status;
        return collection_type_ {receiver_, found};
    }

    /** Opens the transaction, or clears it for reuse, keeping its memory and starting a new generation. */
    status_t reset() noexcept {
        ustore_outcome_t outcome {};
        ustore_transaction_init_t transaction_init {};
        transaction_init.transaction = &receiver_.transaction;
        transaction_init.database = receiver_.database;
        transaction_init.executor = receiver_.executor;
        transaction_init.outcome = &outcome;

        ustore_transaction_init(&transaction_init);
        return status_t {outcome};
    }

    /** Stages the transaction as the first phase of a two-phase commit, answering the stamp it reads at. */
    expected<read_stamp_t> stage() noexcept {
        ustore_outcome_t outcome {};
        ustore_stamp_t read_stamp = 0;
        ustore_transaction_stage_t transaction_stage {};
        transaction_stage.database = receiver_.database;
        transaction_stage.executor = receiver_.executor;
        transaction_stage.outcome = &outcome;
        transaction_stage.transaction = receiver_.transaction;
        transaction_stage.read_stamp = &read_stamp;

        ustore_transaction_stage(&transaction_stage);
        if (status_t const status {outcome}; status.failed()) return status;
        return read_stamp_t {read_stamp};
    }

    /** Withdraws a stage, releasing what it claimed while keeping the reads and writes, so the transaction can stage
     *  or commit again. */
    status_t rollback() noexcept {
        ustore_outcome_t outcome {};
        ustore_transaction_rollback_t transaction_rollback {};
        transaction_rollback.database = receiver_.database;
        transaction_rollback.executor = receiver_.executor;
        transaction_rollback.outcome = &outcome;
        transaction_rollback.transaction = receiver_.transaction;

        ustore_transaction_rollback(&transaction_rollback);
        return status_t {outcome};
    }

    /**
     *  Commits every update of the transaction, or none of them if any one fails, answering the commit's position in
     *  commit order.
     *  @param[in] durability Whether the call returns only after the data reaches the disk.
     */
    expected<commit_stamp_t> commit(ustore_durability_t durability = ustore_durability_buffered_k) noexcept {
        ustore_outcome_t outcome {};
        ustore_stamp_t commit_stamp = 0;
        ustore_transaction_commit_t transaction_commit {};
        transaction_commit.database = receiver_.database;
        transaction_commit.executor = receiver_.executor;
        transaction_commit.outcome = &outcome;
        transaction_commit.transaction = receiver_.transaction;
        transaction_commit.commit_stamp = &commit_stamp;
        transaction_commit.durability = durability;

        ustore_transaction_commit(&transaction_commit);
        if (status_t const status {outcome}; status.failed()) return status;
        return commit_stamp_t {commit_stamp};
    }
};

/** A transaction, which owns its handle and serves one call at a time. It never commits on destruction, never copies,
 *  and holds its database open until destroyed, so it may outlive its @c database_t. */
class transaction_t : public transaction_ref_t {
  public:
    transaction_t() noexcept = default;
    transaction_t(transaction_t const&) = delete;
    transaction_t& operator=(transaction_t const&) = delete;
    transaction_t(transaction_t&& other) noexcept : transaction_ref_t(std::exchange(other.receiver_, {})) {}

    transaction_t& operator=(transaction_t&& other) noexcept {
        std::swap(receiver_, other.receiver_);
        return *this;
    }

    ~transaction_t() noexcept { ustore_transaction_free(receiver_.transaction); }

    /** Begins a transaction of the database @p receiver names, running its calls on the executor it names. */
    static expected<transaction_t> make(receiver_t receiver) noexcept {
        transaction_t transaction;
        transaction.receiver_ = receiver;
        if (status_t const begun = transaction.reset(); begun.failed()) return begun;
        return transaction;
    }
};

#pragma endregion Transactions

#pragma region Snapshots

/** A point-in-time view of the whole database, as the calls on one executor see it, without owning either. It hands
 *  out read-only collections and proxies. */
class snapshot_ref_t {
  protected:
    receiver_t receiver_ {};

  public:
    snapshot_ref_t() noexcept = default;
    explicit snapshot_ref_t(receiver_t receiver) noexcept : receiver_(receiver) {}

    ustore_snapshot_t snapshot() const noexcept { return receiver_.snapshot; }
    receiver_t receiver() const noexcept { return receiver_; }

    /** The same snapshot, running every call on @p executor. */
    snapshot_ref_t on(ustore_executor_t executor) const noexcept { return snapshot_ref_t {receiver_.on(executor)}; }

    /** The same snapshot, whose verbs submit to @p executor and answer deferred calls. */
    deferred_snapshot_t deferred(ustore_executor_t executor) const noexcept {
        return deferred_snapshot_t {receiver_.on(executor)};
    }

    blobs_read_ref<place_batch_t> operator[](strided_span<collection_key_t const> collections_and_keys) noexcept {
        return {receiver_, places_of_(collections_and_keys)};
    }

    blobs_read_ref<place_batch_t> operator[](strided_span<collection_key_field_t const> collections_and_keys) noexcept {
        return {receiver_, places_of_(collections_and_keys)};
    }

    template <typename keys_argument_type_>
        requires has_collection_concept<batch_element_of<keys_argument_type_>>
    blobs_read_ref<keys_argument_type_> operator[](keys_argument_type_&& keys) noexcept {
        return blobs_read_ref<keys_argument_type_> {receiver_, std::forward<keys_argument_type_>(keys)};
    }

    expected<blobs_read_collection_t> operator[](std::string_view name) noexcept { return find(name); }

    /** Lists the collections at the snapshot, with their identifiers in @p arena. */
    expected<collections_list_t> collections(arena_t& arena) noexcept { return list_collections(receiver_, arena); }

    /** Whether a collection named @p name exists at the snapshot. */
    expected<bool> contains(std::string_view name) noexcept { return collection_exists(receiver_, name); }

    /**
     *  @brief Opens a read-only view of one collection at the snapshot.
     *
     *  @tparam collection_type_ The collection type: @c blobs_collection_t, @c docs_collection_t,
     *      as well as @c graph_collection_t or @c strings_collection_t, or their read-only twins.
     */
    template <typename collection_type_ = blobs_collection_t>
    expected<typename collection_type_::read_only_t> find(std::string_view name) noexcept {
        auto [found, lookup_status] = collection_named(receiver_, name, collection_type_::modality_k);
        if (lookup_status.failed()) return lookup_status;
        return typename collection_type_::read_only_t {receiver_, found};
    }

    /** Saves the snapshot as a new database at @p path, reporting the bytes saved to @p progress, which may cancel the
     *  save and so remove what it wrote. */
    status_t save(std::string_view path, progress_arg_t progress = {}) noexcept {
        ustore_outcome_t outcome {};
        ustore_snapshot_save_t snapshot_save {};
        snapshot_save.snapshot = receiver_.snapshot;
        snapshot_save.executor = receiver_.executor;
        snapshot_save.outcome = &outcome;
        snapshot_save.path = bytes_of(path);
        snapshot_save.progress = progress.callback;
        snapshot_save.progress_state = progress.state;
        ustore_snapshot_save(&snapshot_save);
        return status_t {outcome};
    }
};

/** A point-in-time view of the whole database, which reads through it see until its destructor frees it. It owns its
 *  handle, never copies, and holds its database open until destroyed, so it may outlive its @c database_t. */
class snapshot_t : public snapshot_ref_t {
    read_stamp_t read_stamp_ {};

  public:
    snapshot_t() noexcept = default;
    snapshot_t(snapshot_t const&) = delete;
    snapshot_t& operator=(snapshot_t const&) = delete;

    snapshot_t(snapshot_t&& other) noexcept
        : snapshot_ref_t(std::exchange(other.receiver_, {})), read_stamp_(other.read_stamp_) {}

    snapshot_t& operator=(snapshot_t&& other) noexcept {
        std::swap(receiver_, other.receiver_);
        std::swap(read_stamp_, other.read_stamp_);
        return *this;
    }

    ~snapshot_t() noexcept { ustore_snapshot_free(receiver_.snapshot); }

    /** The stamp the snapshot reads at. */
    read_stamp_t read_stamp() const noexcept { return read_stamp_; }

    /** Pins a point-in-time view of the database @p receiver names, running its calls on the executor it names. */
    static expected<snapshot_t> make(receiver_t receiver) noexcept {
        snapshot_t snapshot;
        snapshot.receiver_ = receiver;
        ustore_stamp_t read_stamp = 0;
        ustore_outcome_t outcome {};
        ustore_snapshot_init_t snapshot_init {};
        snapshot_init.snapshot = &snapshot.receiver_.snapshot;
        snapshot_init.database = receiver.database;
        snapshot_init.executor = receiver.executor;
        snapshot_init.outcome = &outcome;
        snapshot_init.read_stamp = &read_stamp;
        ustore_snapshot_init(&snapshot_init);
        if (status_t const status {outcome}; status.failed()) return status;
        snapshot.read_stamp_ = read_stamp_t {read_stamp};
        return snapshot;
    }
};

#pragma endregion Snapshots

#pragma region Databases

/** The head of a database as a map from collection names to collections, as the calls on one executor see it,
 *  without owning either. */
class database_ref_t {
  protected:
    receiver_t receiver_ {};

    ustore_capabilities_t capabilities_() noexcept {
        auto [description, describe_status] = describe();
        return description.capabilities;
    }

  public:
    database_ref_t() noexcept = default;
    explicit database_ref_t(receiver_t receiver) noexcept : receiver_(receiver) {}

    operator ustore_database_t() const noexcept { return receiver_.database; }
    receiver_t receiver() const noexcept { return receiver_; }

    /** The same head, running every call on @p executor. */
    database_ref_t on(ustore_executor_t executor) const noexcept { return database_ref_t {receiver_.on(executor)}; }

    /** The same head, whose verbs submit to @p executor and answer deferred calls. */
    deferred_database_t deferred(ustore_executor_t executor) const noexcept {
        return deferred_database_t {receiver_.on(executor)};
    }

    /** Reports the engine of the database, what it supports and how much it holds. */
    expected<ustore_database_description_t> describe() noexcept {
        ustore_outcome_t outcome {};
        ustore_database_description_t description {};
        ustore_database_describe_t database_describe {};
        database_describe.database = receiver_.database;
        database_describe.executor = receiver_.executor;
        database_describe.outcome = &outcome;
        database_describe.description = &description;
        ustore_database_describe(&database_describe);
        if (status_t const status {outcome}; status.failed()) return status;
        return description;
    }

    bool supports_transactions() noexcept { return capabilities_() & ustore_capabilities_transactions_k; }
    bool supports_snapshots() noexcept { return capabilities_() & ustore_capabilities_snapshots_k; }

    /** Begins a transaction, which runs its calls on this receiver's executor. */
    expected<transaction_t> transaction() noexcept { return transaction_t::make(receiver_); }

    /** Pins a point-in-time view of the database, which the returned snapshot frees when it goes. */
    expected<snapshot_t> snapshot() noexcept { return snapshot_t::make(receiver_); }

    expected<blobs_collection_t> operator[](std::string_view name) noexcept { return find_or_create(name); }

    /** Whether a collection named @p name exists at the head. */
    expected<bool> contains(std::string_view name) noexcept { return collection_exists(receiver_, name); }

    /** Lists the collections at the head, with their identifiers in @p arena. */
    expected<collections_list_t> collections(arena_t& arena) noexcept { return list_collections(receiver_, arena); }

    /** Creates a collection holding the modality of @p collection_type_ under @p name, which no
     *  other collection may carry. */
    template <typename collection_type_ = blobs_collection_t>
    expected<collection_type_> create(std::string_view name) noexcept {
        ustore_outcome_t outcome {};
        ustore_collection_id_t collection {};
        ustore_collection_create_t create {};
        create.database = receiver_.database;
        create.executor = receiver_.executor;
        create.outcome = &outcome;
        create.name = bytes_of(name);
        create.modality = collection_type_::modality_k;
        create.collection = &collection;
        ustore_collection_create(&create);
        if (status_t const status {outcome}; status.failed()) return status;
        return collection_type_ {receiver_, collection};
    }

    /**
     *  @brief The collection named @p name, refusing one that holds another modality than
     *      @p collection_type_.
     *
     *  @par Statuses:
     *  - @c status_code_t::not_found_k No collection carries @p name.
     *  - @c status_code_t::invalid_argument_k The collection holds another modality than
     *    @p collection_type_.
     */
    template <typename collection_type_ = blobs_collection_t>
    expected<collection_type_> find(std::string_view name) noexcept {
        auto [found, find_status] = collection_named(receiver_, name, collection_type_::modality_k);
        if (find_status.failed()) return find_status;
        return collection_type_ {receiver_, found};
    }

    /** The collection named @p name, created when no collection carries the name. */
    template <typename collection_type_ = blobs_collection_t>
    expected<collection_type_> find_or_create(std::string_view name) noexcept {
        expected<collection_type_> found = find<collection_type_>(name);
        if (found || found.code() != status_code_t::not_found_k) return found;
        return create<collection_type_>(name);
    }

    /** Drops the collection @p name addresses, which takes the name with it. */
    status_t drop(std::string_view name) noexcept {
        auto [collection, find_status] = collection_named(receiver_, name);
        if (find_status.failed()) return find_status;

        ustore_outcome_t outcome {};
        ustore_collection_drop_t drop {};
        drop.database = receiver_.database;
        drop.executor = receiver_.executor;
        drop.outcome = &outcome;
        drop.collection = collection;
        drop.mode = ustore_drop_collection_k;
        ustore_collection_drop(&drop);
        return status_t {outcome};
    }

    /**
     *  Empties every collection at once, as @p mode names.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k The RAM and RocksDB engines refuse to drop the collections while a
     *    snapshot is open.
     */
    status_t clear(ustore_drop_mode_t mode) noexcept {
        ustore_outcome_t outcome {};
        ustore_database_clear_t database_clear {};
        database_clear.database = receiver_.database;
        database_clear.executor = receiver_.executor;
        database_clear.outcome = &outcome;
        database_clear.mode = mode;
        ustore_database_clear(&database_clear);
        return status_t {outcome};
    }

    /** Reclaims the space overwritten and removed data still takes, one collection at a time, reporting its advance to
     *  @p progress, which may cancel it between collections. */
    status_t compact(progress_arg_t progress = {}) noexcept {
        ustore_outcome_t outcome {};
        ustore_database_compact_t database_compact {};
        database_compact.database = receiver_.database;
        database_compact.executor = receiver_.executor;
        database_compact.outcome = &outcome;
        database_compact.progress = progress.callback;
        database_compact.progress_state = progress.state;
        ustore_database_compact(&database_compact);
        return status_t {outcome};
    }
};

/** A database as a transactional map from collection names to collections of binary values, which owns its handle.
 *  It is thread-safe except for the opens and @c close, never copies, and outlives the collections it hands out, while
 *  its transactions and snapshots may outlive it. */
class database_t : public database_ref_t {

    /** Opens the engine behind @p init with @p options, blocking on @p executor, whose outputs this call fills in. */
    template <typename options_type_>
    static expected<database_t> open_(void (*init)(options_type_*), options_type_ options,
                                      ustore_executor_t executor) noexcept {
        database_t database;
        ustore_outcome_t outcome {};
        options.database = &database.receiver_.database;
        options.executor = executor;
        options.outcome = &outcome;
        options.completion = ustore_completion_blocking_k;
        init(&options);
        if (status_t const status {outcome}; status.failed()) return status;
        return database;
    }

  public:
    database_t() noexcept = default;
    database_t(database_t const&) = delete;
    database_t& operator=(database_t const&) = delete;
    database_t(database_t&& other) noexcept : database_ref_t(std::exchange(other.receiver_, {})) {}
    explicit database_t(ustore_database_t adopted) noexcept : database_ref_t(receiver_t {adopted}) {}
    database_t& operator=(database_t&& other) noexcept {
        std::swap(receiver_, other.receiver_);
        return *this;
    }

    /** Starts an empty database of the in-memory engine blocking on @p executor, or on the calling thread without one.
     *  The database runs its own calls on the calling thread until a receiver names an executor. */
    static expected<database_t> init_ram(ustore_database_init_ram_t options = {},
                                         ustore_executor_t executor = nullptr) noexcept {
        return open_(ustore_database_init_ram, options, executor);
    }

    /** Serves the directory @p options name through the in-memory engine, which persists there as one image,
     *  blocking on @p executor, or on the calling thread without one. */
    static expected<database_t> open_ram(ustore_database_open_ram_t options,
                                         ustore_executor_t executor = nullptr) noexcept {
        return open_(ustore_database_open_ram, options, executor);
    }

    /** Serves the directory @p options name through the RocksDB engine, blocking on @p executor, or on the calling
     *  thread without one. */
    static expected<database_t> open_rocksdb(ustore_database_open_rocksdb_t options,
                                             ustore_executor_t executor = nullptr) noexcept {
        return open_(ustore_database_open_rocksdb, options, executor);
    }

    /** Serves the directory @p options name through the RocksDB engine, read-only, blocking on @p executor, or on the
     *  calling thread without one. */
    static expected<database_t> view_rocksdb(ustore_database_view_rocksdb_t options,
                                             ustore_executor_t executor = nullptr) noexcept {
        return open_(ustore_database_view_rocksdb, options, executor);
    }

    /** Serves the directory @p options name through the LevelDB engine, blocking on @p executor, or on the calling
     *  thread without one. */
    static expected<database_t> open_leveldb(ustore_database_open_leveldb_t options,
                                             ustore_executor_t executor = nullptr) noexcept {
        return open_(ustore_database_open_leveldb, options, executor);
    }

    /** Serves the directories @p options name through the NVMe engine on the calling thread, lending it the executor
     *  they name until close. */
    static expected<database_t> open_nvme(ustore_database_open_nvme_t options) noexcept {
        return open_(ustore_database_open_nvme, options, options.executor);
    }

    /** Drops this handle, closing the database once no transaction or snapshot of it holds it open. */
    void close() noexcept {
        ustore_database_free(receiver_.database);
        receiver_ = {};
    }

    ~database_t() noexcept {
        if (receiver_.database) close();
    }
};

#pragma endregion Databases

} // namespace unum::ustore
