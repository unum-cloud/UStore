/**
 *  @file cpp/ustore/db.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief The database, transaction and snapshot handles of the C++ wrappers, the receivers that
 *      run their calls on an executor, blocking or deferred, and the feeds of changes and ingests.
 *
 *  A handle owns its C handle and its lifetime, and a receiver runs its verbs: @c on blocks on an
 *  executor, or on the calling thread without one, answering an @c outcome, and @c deferred
 *  submits to an executor, answering a @c deferred call. Each verb is written once, as a template
 *  on the completion. Snapshots hand out read-only collections and proxies, so nothing writes
 *  through a snapshot.
 *
 *  @code{.cpp}
 *  namespace us = unum::ustore;
 *  auto [database, open_status] = us::database_t::init_ram();
 *  auto [executor, executor_status] = us::executor_handle_t::for_ram({.threads = 4});
 *  auto [collection, create_status] = database.on().create("blobs");
 *  us::deferred<void> const written = collection.deferred(executor).write(keys, values);
 *  @endcode
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <iterator>    // `std::size`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::exchange`, `std::forward`, `std::swap`

#include "ustore/blobs_collection.hpp"   // `blobs_collection_on`, `blobs_collection_t`
#include "ustore/blobs_ref.hpp"          // `blobs_ref`, `blobs_read_ref`
#include "ustore/docs_collection.hpp"    // what `find` and `create` take as a template argument
#include "ustore/graph_collection.hpp"   // what `find` and `create` take as a template argument
#include "ustore/pages_collection.hpp"   // what `find` and `create` take as a template argument
#include "ustore/ranges.hpp"             // `strided_span`, `place_batch_t`, `locations_places_`, `edges_view_t`
#include "ustore/status.hpp"             // `status_t`, `expected`, `answer`, `called_`, `defers`
#include "ustore/strings_collection.hpp" // what `find` and `create` take as a template argument
#include "ustore/types.hpp"              // `arena_t`, `receiver_t`, `read_stamp_t`, `commit_stamp_t`, `progress_arg_t`
#include "ustore/ustore.h"

namespace unum::ustore {

using collections_list_t = std::span<ustore_collection_id_t const>;

/** Where a listing writes the identifiers of the collections and how many there are. */
template <>
struct outputs<collections_list_t> {

    /** Output for the identifiers, inside the arena. */
    ustore_collection_id_t const* collections = nullptr;

    /** Output for the collections listed. */
    ustore_size_t written = 0;

    collections_list_t answer() const noexcept { return {collections, collections ? written : 0}; }
};

#pragma region Lookups

/**
 *  @brief Reads the collection @p name was given at the head, or at the snapshot of @p receiver
 *      when it names one, through a one-off arena, since a name answers one identifier.
 *
 *  Names resolve in two phases, so this is the only place a name reaches the database, and every
 *  other call addresses its collection by the identifier this returns. It blocks whatever the
 *  receiver completes as.
 *
 *  @param[in] receiver The receiver whose snapshot the name is read at, or the head when
 *      it names none.
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

/** Lists the collections at the head, or at the snapshot of @p receiver, with their identifiers in
 *  @p arena, borrowed until the call is final. */
template <ustore_completion_t completion_>
answer<completion_, collections_list_t> collections_list_(receiver_t const& receiver, arena_t& arena) noexcept {
    return called_<completion_, collections_list_t>(
        [&](ustore_outcome_t* outcome, outputs<collections_list_t>* written) noexcept {
            ustore_collection_list_t list {};
            list.database = receiver.database;
            list.executor = receiver.executor;
            list.outcome = outcome;
            list.completion = completion_;
            list.snapshot = receiver.snapshot;
            list.collections = &written->collections;
            list.written = &written->written;
            list.arena = arena;
            ustore_collection_list(&list);
        });
}

#pragma endregion Lookups

#pragma region Transactions

/**
 *  @brief A transaction's receiver, which reads and writes through it and completes its verbs as
 *      @p completion_ says.
 *
 *  Batched writes are buffered in the layout the engine prefers until the commit, so a transaction
 *  also saves preprocessing. A transaction serves one call at a time, so a caller waits on one
 *  deferred call before submitting the next.
 */
template <ustore_completion_t completion_>
class transaction_on {
    receiver_t receiver_ {};

    static constexpr bool blocks_k = !defers<completion_>;

  public:
    transaction_on() noexcept = default;
    explicit transaction_on(receiver_t receiver) noexcept : receiver_(receiver) {}

    ustore_transaction_t transaction() const noexcept { return receiver_.transaction; }
    receiver_t receiver() const noexcept { return receiver_; }
    operator ustore_transaction_t() const noexcept { return receiver_.transaction; }

    /** The same transaction, whose verbs block on @p executor, or on the calling thread without
     *  one. */
    transaction_on<ustore_completion_blocking_k> on(ustore_executor_t executor) const noexcept {
        return transaction_on<ustore_completion_blocking_k> {receiver_.on(executor)};
    }

    /** The same transaction, whose verbs submit to @p executor and answer deferred calls. */
    transaction_on<ustore_completion_deferred_k> deferred(ustore_executor_t executor) const noexcept {
        return transaction_on<ustore_completion_deferred_k> {receiver_.on(executor)};
    }

    /**
     *  @brief Opens a view of one collection that follows this transaction, completing as
     *      this receiver does.
     *
     *  @tparam collection_type_ The collection type: @c blobs_collection_t, @c docs_collection_t,
     *      as well as @c graph_collection_t, @c pages_collection_t or @c strings_collection_t.
     */
    template <typename collection_type_ = blobs_collection_t>
    expected<typename collection_type_::template completed_as<completion_>> find(std::string_view name) const noexcept {
        auto [found, lookup_status] = collection_named(receiver_, name, collection_type_::modality_k);
        if (lookup_status.failed()) return lookup_status;
        return typename collection_type_::template completed_as<completion_> {receiver_, found};
    }

    expected<blobs_collection_on<completion_, collection_access_t::read_write_k>> operator[](
        std::string_view name) const noexcept {
        return find(name);
    }

    /** Whether a collection named @p name exists at the head. */
    expected<bool> contains(std::string_view name) const noexcept { return collection_exists(receiver_, name); }

    blobs_ref<place_batch_t> operator[](strided_span<collection_key_t const> collections_and_keys) const noexcept
        requires blocks_k
    {
        return {receiver_, locations_places_(collections_and_keys)};
    }

    blobs_ref<place_batch_t> operator[](strided_span<collection_key_field_t const> collections_and_keys) const noexcept
        requires blocks_k
    {
        return {receiver_, locations_places_(collections_and_keys)};
    }

    template <typename keys_argument_type_>
        requires has_collection_concept<batch_element_of<keys_argument_type_>>
    blobs_ref<keys_argument_type_> operator[](keys_argument_type_&& keys) const noexcept
        requires blocks_k
    {
        return blobs_ref<keys_argument_type_> {receiver_, std::forward<keys_argument_type_>(keys)};
    }

#pragma region Verbs

    /** Opens the transaction, or clears it for reuse, keeping its memory and starting a new
     *  generation, answering the handle, which reuse keeps. */
    answer<completion_, ustore_transaction_t> reset() const noexcept {
        return called_<completion_, ustore_transaction_t>(
            [&](ustore_outcome_t* outcome, outputs<ustore_transaction_t>* written) noexcept {
                written->value = receiver_.transaction;
                ustore_transaction_init_t init {};
                init.transaction = &written->value;
                init.database = receiver_.database;
                init.executor = receiver_.executor;
                init.outcome = outcome;
                init.completion = completion_;
                ustore_transaction_init(&init);
            });
    }

    /** Stages the transaction as the first phase of a two-phase commit, answering the stamp it
     *  reads at. */
    answer<completion_, read_stamp_t> stage() const noexcept {
        return called_<completion_, read_stamp_t>(
            [&](ustore_outcome_t* outcome, outputs<read_stamp_t>* written) noexcept {
                ustore_transaction_stage_t stage {};
                stage.database = receiver_.database;
                stage.executor = receiver_.executor;
                stage.outcome = outcome;
                stage.completion = completion_;
                stage.transaction = receiver_.transaction;
                stage.read_stamp = &written->stamp;
                ustore_transaction_stage(&stage);
            });
    }

    /** Withdraws a stage, releasing what it claimed while keeping the reads and writes, so the
     *  transaction can stage or commit again. */
    answer<completion_, void> rollback() const noexcept {
        return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            ustore_transaction_rollback_t rollback {};
            rollback.database = receiver_.database;
            rollback.executor = receiver_.executor;
            rollback.outcome = outcome;
            rollback.completion = completion_;
            rollback.transaction = receiver_.transaction;
            ustore_transaction_rollback(&rollback);
        });
    }

    /**
     *  @brief Commits every update of the transaction, or none of them if any one fails, answering
     *      the commit's position in commit order.
     *  @param[in] durability Whether the call returns only after the data reaches the disk.
     */
    answer<completion_, commit_stamp_t> commit(
        ustore_durability_t durability = ustore_durability_buffered_k) const noexcept {
        return called_<completion_, commit_stamp_t>(
            [&](ustore_outcome_t* outcome, outputs<commit_stamp_t>* written) noexcept {
                ustore_transaction_commit_t commit {};
                commit.database = receiver_.database;
                commit.executor = receiver_.executor;
                commit.outcome = outcome;
                commit.completion = completion_;
                commit.transaction = receiver_.transaction;
                commit.commit_stamp = &written->stamp;
                commit.durability = durability;
                ustore_transaction_commit(&commit);
            });
    }

#pragma endregion Verbs
};

/** A transaction whose verbs block, as the calls on one executor see it, without owning either. */
using transaction_ref_t = transaction_on<ustore_completion_blocking_k>;

/** A transaction, which owns its handle and serves one call at a time. It never commits on
 *  destruction, never copies, and holds its database open until destroyed, so it may outlive its
 *  @c database_t. Its verbs run through @c on and @c deferred. */
class transaction_t {
    receiver_t receiver_ {};

  public:
    transaction_t() noexcept = default;
    transaction_t(transaction_t const&) = delete;
    transaction_t& operator=(transaction_t const&) = delete;
    transaction_t(transaction_t&& other) noexcept : receiver_(std::exchange(other.receiver_, {})) {}
    transaction_t& operator=(transaction_t&& other) noexcept {
        std::swap(receiver_, other.receiver_);
        return *this;
    }
    ~transaction_t() noexcept { ustore_transaction_free(receiver_.transaction); }

    ustore_transaction_t transaction() const noexcept { return receiver_.transaction; }
    receiver_t receiver() const noexcept { return receiver_; }
    operator ustore_transaction_t() const noexcept { return receiver_.transaction; }

    /** The receiver whose verbs block on the executor the transaction was begun on. */
    transaction_ref_t on() const noexcept { return transaction_ref_t {receiver_}; }

    /** The receiver whose verbs block on @p executor, or on the calling thread without one. */
    transaction_ref_t on(ustore_executor_t executor) const noexcept {
        return transaction_ref_t {receiver_.on(executor)};
    }

    /** The receiver whose verbs submit to @p executor and answer deferred calls. */
    transaction_on<ustore_completion_deferred_k> deferred(ustore_executor_t executor) const noexcept {
        return transaction_on<ustore_completion_deferred_k> {receiver_.on(executor)};
    }

    /** Begins a transaction of the database @p receiver names, running its calls on the executor it
     *  names. */
    static expected<transaction_t> make(receiver_t receiver) noexcept {
        auto [opened, open_status] = transaction_ref_t {receiver}.reset();
        if (open_status.failed()) return open_status;
        transaction_t transaction;
        transaction.receiver_ = receiver;
        transaction.receiver_.transaction = opened;
        return transaction;
    }
};

#pragma endregion Transactions

#pragma region Snapshots

/** A snapshot's receiver, a point-in-time view of the whole database that completes its verbs as @p
 *  completion_ says. It hands out read-only collections and proxies. */
template <ustore_completion_t completion_>
class snapshot_on {
    receiver_t receiver_ {};

    static constexpr bool blocks_k = !defers<completion_>;

  public:
    snapshot_on() noexcept = default;
    explicit snapshot_on(receiver_t receiver) noexcept : receiver_(receiver) {}

    ustore_snapshot_t snapshot() const noexcept { return receiver_.snapshot; }
    receiver_t receiver() const noexcept { return receiver_; }

    /** The same snapshot, whose verbs block on @p executor, or on the calling
     *  thread without one. */
    snapshot_on<ustore_completion_blocking_k> on(ustore_executor_t executor) const noexcept {
        return snapshot_on<ustore_completion_blocking_k> {receiver_.on(executor)};
    }

    /** The same snapshot, whose verbs submit to @p executor and answer deferred calls. */
    snapshot_on<ustore_completion_deferred_k> deferred(ustore_executor_t executor) const noexcept {
        return snapshot_on<ustore_completion_deferred_k> {receiver_.on(executor)};
    }

    /**
     *  @brief Opens a read-only view of one collection at the snapshot, completing as
     *      this receiver does.
     *
     *  @tparam collection_type_ The collection type: @c blobs_collection_t, @c docs_collection_t,
     *      as well as @c graph_collection_t, @c pages_collection_t or @c strings_collection_t, or
     *      their read-only twins.
     */
    template <typename collection_type_ = blobs_collection_t>
    expected<typename collection_type_::read_only_t::template completed_as<completion_>> find(
        std::string_view name) const noexcept {
        auto [found, lookup_status] = collection_named(receiver_, name, collection_type_::modality_k);
        if (lookup_status.failed()) return lookup_status;
        return typename collection_type_::read_only_t::template completed_as<completion_> {receiver_, found};
    }

    expected<blobs_collection_on<completion_, collection_access_t::read_only_k>> operator[](
        std::string_view name) const noexcept {
        return find(name);
    }

    /** Whether a collection named @p name exists at the snapshot. */
    expected<bool> contains(std::string_view name) const noexcept { return collection_exists(receiver_, name); }

    blobs_read_ref<place_batch_t> operator[](strided_span<collection_key_t const> collections_and_keys) const noexcept
        requires blocks_k
    {
        return {receiver_, locations_places_(collections_and_keys)};
    }

    blobs_read_ref<place_batch_t> operator[](
        strided_span<collection_key_field_t const> collections_and_keys) const noexcept
        requires blocks_k
    {
        return {receiver_, locations_places_(collections_and_keys)};
    }

    template <typename keys_argument_type_>
        requires has_collection_concept<batch_element_of<keys_argument_type_>>
    blobs_read_ref<keys_argument_type_> operator[](keys_argument_type_&& keys) const noexcept
        requires blocks_k
    {
        return blobs_read_ref<keys_argument_type_> {receiver_, std::forward<keys_argument_type_>(keys)};
    }

#pragma region Verbs

    /** Lists the collections at the snapshot, with their identifiers in @p arena, borrowed until
     *  the call is final. */
    answer<completion_, collections_list_t> collections(arena_t& arena) const noexcept {
        return collections_list_<completion_>(receiver_, arena);
    }

    /** Saves the snapshot as a new database at @p path, borrowed until the call is final, reporting
     *  the bytes saved to @p progress, which may cancel the save and so remove what it wrote. */
    answer<completion_, void> save(std::string_view path, progress_arg_t progress = {}) const noexcept {
        return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            ustore_snapshot_save_t save {};
            save.snapshot = receiver_.snapshot;
            save.executor = receiver_.executor;
            save.outcome = outcome;
            save.completion = completion_;
            save.path = bytes_of(path);
            save.progress = progress.callback;
            save.progress_state = progress.state;
            ustore_snapshot_save(&save);
        });
    }

#pragma endregion Verbs
};

/** A snapshot whose verbs block, as the calls on one executor see it, without owning either. */
using snapshot_ref_t = snapshot_on<ustore_completion_blocking_k>;

/** A point-in-time view of the whole database, which reads through it see until its destructor
 *  frees it. It owns its handle, never copies, and holds its database open until destroyed, so it
 *  may outlive its @c database_t. Its verbs run through @c on and @c deferred. */
class snapshot_t {
    receiver_t receiver_ {};
    read_stamp_t read_stamp_ {};

  public:
    snapshot_t() noexcept = default;
    snapshot_t(snapshot_t const&) = delete;
    snapshot_t& operator=(snapshot_t const&) = delete;
    snapshot_t(snapshot_t&& other) noexcept
        : receiver_(std::exchange(other.receiver_, {})), read_stamp_(other.read_stamp_) {}
    snapshot_t& operator=(snapshot_t&& other) noexcept {
        std::swap(receiver_, other.receiver_);
        std::swap(read_stamp_, other.read_stamp_);
        return *this;
    }
    ~snapshot_t() noexcept { ustore_snapshot_free(receiver_.snapshot); }

    ustore_snapshot_t snapshot() const noexcept { return receiver_.snapshot; }
    receiver_t receiver() const noexcept { return receiver_; }

    /** The stamp the snapshot reads at. */
    read_stamp_t read_stamp() const noexcept { return read_stamp_; }

    /** The receiver whose verbs block on the executor the snapshot was pinned on. */
    snapshot_ref_t on() const noexcept { return snapshot_ref_t {receiver_}; }

    /** The receiver whose verbs block on @p executor, or on the calling thread without one. */
    snapshot_ref_t on(ustore_executor_t executor) const noexcept { return snapshot_ref_t {receiver_.on(executor)}; }

    /** The receiver whose verbs submit to @p executor and answer deferred calls. */
    snapshot_on<ustore_completion_deferred_k> deferred(ustore_executor_t executor) const noexcept {
        return snapshot_on<ustore_completion_deferred_k> {receiver_.on(executor)};
    }

    /** Pins a point-in-time view of the database @p receiver names, running its calls on the
     *  executor it names. */
    static expected<snapshot_t> make(receiver_t receiver) noexcept {
        snapshot_t snapshot;
        snapshot.receiver_ = receiver;
        ustore_stamp_t read_stamp = 0;
        ustore_outcome_t outcome {};
        ustore_snapshot_init_t init {};
        init.snapshot = &snapshot.receiver_.snapshot;
        init.database = receiver.database;
        init.executor = receiver.executor;
        init.outcome = &outcome;
        init.read_stamp = &read_stamp;
        ustore_snapshot_init(&init);
        if (status_t const status {outcome}; status.failed()) return status;
        snapshot.read_stamp_ = read_stamp_t {read_stamp};
        return snapshot;
    }
};

#pragma endregion Snapshots

#pragma region Changes

/** The changes one pull of a feed handed out, oldest first, inside the arena it was given. */
struct changes_t {

    /** The key every change touched. */
    std::span<ustore_key_t const> keys;

    /** The stamp every change was committed at. */
    std::span<ustore_stamp_t const> stamps;

    /** What every change did, which @c ustore_change_kind_name spells. */
    std::span<ustore_change_kind_t const> kinds;

    /** The bytes every change carries, empty for a removal, or none when the pull read none. */
    sz::strings_view_t values {strings_of({}, 0)};
};

/**
 *  @brief Owns a feed of the changes committed to one collection after a stamp, oldest first,
 *      from its open until its destruction, which every pull resumes past the last change the
 *      pull before it handed out.
 *
 *  It pins the stamp it reads from, as a snapshot does, and holds its database open until it is
 *  destroyed. Every pull takes the caller's arena, which holds the changes until its next call,
 *  and a pull that fails leaves the feed where it was. It moves but never copies.
 */
class changes_cursor_t {

    receiver_t receiver_ {};
    ustore_changes_cursor_t cursor_ = nullptr;

    changes_cursor_t(receiver_t receiver, ustore_changes_cursor_t cursor) noexcept
        : receiver_(receiver), cursor_(cursor) {}

    /** Pulls up to @p wanted changes into @p arena, with their bytes into @p values unless it is
     *  null. */
    expected<changes_t> pull_(std::size_t wanted, arena_t& arena, ustore_strings_view_t* values) noexcept {
        ustore_size_t count = 0;
        ustore_key_t const* keys = nullptr;
        ustore_stamp_t const* stamps = nullptr;
        ustore_change_kind_t const* kinds = nullptr;
        ustore_outcome_t outcome {};
        ustore_changes_cursor_next_t next {};
        next.cursor = cursor_;
        next.executor = receiver_.executor;
        next.outcome = &outcome;
        next.wanted = wanted;
        next.count = &count;
        next.keys = &keys;
        next.stamps = &stamps;
        next.kinds = &kinds;
        next.values = values;
        next.arena = arena;
        ustore_changes_cursor_next(&next);
        if (status_t const status {outcome}; status.failed()) return status;
        std::size_t const pulled = static_cast<std::size_t>(count);
        changes_t changes {{keys, pulled}, {stamps, pulled}, {kinds, pulled}};
        if (values) changes.values = strings_of(*values, pulled);
        return changes;
    }

  public:
    changes_cursor_t() noexcept = default;
    changes_cursor_t(changes_cursor_t const&) = delete;
    changes_cursor_t& operator=(changes_cursor_t const&) = delete;
    ~changes_cursor_t() noexcept { ustore_changes_cursor_free(cursor_); }

    changes_cursor_t(changes_cursor_t&& other) noexcept
        : receiver_(other.receiver_), cursor_(std::exchange(other.cursor_, nullptr)) {}

    changes_cursor_t& operator=(changes_cursor_t&& other) noexcept {
        std::swap(receiver_, other.receiver_);
        std::swap(cursor_, other.cursor_);
        return *this;
    }

    /**
     *  @brief Opens a feed of the changes committed to @p collection after @p since, on the
     *      database and the executor @p receiver names, reading nothing until its first pull.
     *
     *  @par Statuses:
     *  - @c status_code_t::not_found_k The collection does not exist.
     *  - @c status_code_t::unsupported_k The executor computes on a device, or the engine keeps
     *    no stamps, as LevelDB does.
     *  - @c status_code_t::write_conflict_k The versions after @p since are already gone, or the
     *    RAM engine is asked for a stamp below its head that no open feed pins.
     */
    static expected<changes_cursor_t> make(receiver_t receiver, ustore_collection_id_t collection,
                                           commit_stamp_t since = {}) noexcept {
        ustore_changes_cursor_t cursor = nullptr;
        ustore_outcome_t outcome {};
        ustore_changes_cursor_init_t init {};
        init.database = receiver.database;
        init.executor = receiver.executor;
        init.outcome = &outcome;
        init.collection = collection;
        init.since_stamp = static_cast<ustore_stamp_t>(since);
        init.cursor = &cursor;
        ustore_changes_cursor_init(&init);
        if (status_t const status {outcome}; status.failed()) return status;
        return changes_cursor_t {receiver, cursor};
    }

    /** Pulls up to @p wanted next changes into @p arena, none once the feed is at its newest
     *  commit. */
    expected<changes_t> next(std::size_t wanted, arena_t& arena) noexcept { return pull_(wanted, arena, nullptr); }

    /** Pulls up to @p wanted next changes with the bytes every one carries into @p arena in one
     *  call. */
    expected<changes_t> next_pairs(std::size_t wanted, arena_t& arena) noexcept {
        ustore_strings_view_t values {};
        return pull_(wanted, arena, &values);
    }
};

#pragma endregion Changes

#pragma region Ingests

/**
 *  @brief Owns an ingest under way into one collection, which lands every row appended to it as one
 *      commit of its own, from its open until its finish, and frees it on destruction, dropping the
 *      rows of an unfinished one.
 *
 *  It joins no transaction and watches nothing, so a reader sees all of the ingest or none of it.
 *  Its calls run on the calling thread, one at a time, and copy their rows before returning, so
 *  every input may die after the call. It moves but never copies.
 */
class ingest_t {
    ustore_ingest_t ingest_ = nullptr;

  public:
    ingest_t() noexcept = default;
    ingest_t(ingest_t const&) = delete;
    ingest_t& operator=(ingest_t const&) = delete;
    ingest_t(ingest_t&& other) noexcept : ingest_(std::exchange(other.ingest_, nullptr)) {}
    ingest_t& operator=(ingest_t&& other) noexcept {
        std::swap(ingest_, other.ingest_);
        return *this;
    }
    ~ingest_t() noexcept { ustore_ingest_free(ingest_); }

    operator ustore_ingest_t() const noexcept { return ingest_; }

    /**
     *  @brief Opens an ingest into @p collection of @p database, whose rows arrive in @p order.
     *
     *  @par Statuses:
     *  - @c status_code_t::read_only_k The database was opened read-only.
     *  - @c status_code_t::unsupported_k The engine ingests nothing, or not into the collection's
     *    modality.
     *  - @c status_code_t::not_found_k The collection does not exist.
     */
    static expected<ingest_t> make(ustore_database_t database, ustore_collection_id_t collection,
                                   ustore_ingest_order_t order = ustore_ingest_sorted_k,
                                   ustore_executor_t executor = nullptr) noexcept {
        ingest_t ingest;
        ustore_outcome_t outcome {};
        ustore_ingest_init_t init {};
        init.ingest = &ingest.ingest_;
        init.database = database;
        init.executor = executor;
        init.outcome = &outcome;
        init.collection = collection;
        init.order = order;
        ustore_ingest_init(&init);
        if (status_t const status {outcome}; status.failed()) return status;
        return ingest;
    }

    /**
     *  @brief Hands the ingest one batch of @p values beside their @p keys, pairwise: the values
     *      of blobs, JSON documents and XML pages, or the strings of a strings collection beside
     *      the keys they map onto.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k The @p keys and @p values differ in count, the ingest
     *    is finished, or a key does not ascend past every key before it under
     *    @c ustore_ingest_sorted_k.
     *  - @c status_code_t::malformed_k A value is past what the collection admits, a document no
     *    JSON, or a page no XML.
     */
    template <strings_batch_concept values_type_>
    status_t append(keys_view_t keys, values_type_ const& values, ustore_executor_t executor = nullptr) noexcept {
        // The call has one count, so only this side sees the keys and the values disagree
        if (keys.size() != std::size(values))
            return status_t {status_code_t::invalid_argument_k, "Every key needs its own value"};
        ustore_outcome_t outcome {};
        ustore_ingest_append_t append {};
        append.ingest = ingest_;
        append.executor = executor;
        append.outcome = &outcome;
        append.count = keys.size();
        append.keys = {keys.data(), keys.stride()};
        append.values = strings_view_of(values);
        ustore_ingest_append(&append);
        return status_t {outcome};
    }

    /** Hands the ingest of a graph collection one batch of @p edges on @p executor, refusing what
     *  the other @c append refuses. */
    status_t append(edges_view_t const& edges, ustore_executor_t executor = nullptr) noexcept {
        ustore_outcome_t outcome {};
        ustore_ingest_append_t append {};
        append.ingest = ingest_;
        append.executor = executor;
        append.outcome = &outcome;
        append.count = edges.size();
        append.sources = {edges.sources.data(), edges.sources.stride()};
        append.targets = {edges.targets.data(), edges.targets.stride()};
        append.edges = {edges.ids.data(), edges.ids.stride()};
        ustore_ingest_append(&append);
        return status_t {outcome};
    }

    /**
     *  @brief Lands every row appended as one commit and ends the ingest, whatever it answers,
     *      answering the one stamp every row landed at.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k The ingest is finished.
     *  - @c status_code_t::not_found_k The collection was dropped meanwhile.
     *  - @c status_code_t::contended_k Memory runs kept refilling while the ingest waited them out.
     */
    expected<commit_stamp_t> finish(ustore_durability_t durability = ustore_durability_buffered_k,
                                    ustore_executor_t executor = nullptr) noexcept {
        ustore_stamp_t stamp = 0;
        ustore_outcome_t outcome {};
        ustore_ingest_finish_t finish {};
        finish.ingest = ingest_;
        finish.executor = executor;
        finish.outcome = &outcome;
        finish.durability = durability;
        finish.commit_stamp = &stamp;
        ustore_ingest_finish(&finish);
        if (status_t const status {outcome}; status.failed()) return status;
        return commit_stamp_t {stamp};
    }
};

#pragma endregion Ingests

#pragma region Databases

template <ustore_completion_t completion_>
class database_on;

/**
 *  @brief A compaction in flight, which @c compact opens on its first call and continues on every
 *      later one, until @c compact_plan_finish consumes it.
 *
 *  Nothing finishes it on destruction: a dropped live plan leaks and keeps its database closed to
 *  another compaction, as an unfinished plan does in C. The database and the executor it was opened
 *  with outlive every call naming it and its finish.
 */
class database_compact_plan_t {
    ustore_database_compact_plan_t handle_ = nullptr;
    ustore_phase_progress_t phase_ {};

    template <ustore_completion_t>
    friend class database_on;

  public:
    database_compact_plan_t() noexcept = default;
    database_compact_plan_t(database_compact_plan_t&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)), phase_(other.phase_) {}
    database_compact_plan_t& operator=(database_compact_plan_t&& other) noexcept {
        std::swap(handle_, other.handle_);
        std::swap(phase_, other.phase_);
        return *this;
    }
    database_compact_plan_t(database_compact_plan_t const&) = delete;
    database_compact_plan_t& operator=(database_compact_plan_t const&) = delete;

    /** Where the last call naming the plan stopped; @c ustore_phase_name spells its phase. */
    ustore_phase_progress_t phase() const noexcept { return phase_; }

    ustore_database_compact_plan_t handle() const noexcept { return handle_; }
};

/** The head of a database as a map from collection names to collections, whose verbs complete as @p
 *  completion_ says, without owning the database or the executor. */
template <ustore_completion_t completion_>
class database_on {
    receiver_t receiver_ {};

    static constexpr bool blocks_k = !defers<completion_>;

    ustore_capabilities_t capabilities_() const noexcept
        requires blocks_k
    {
        auto [description, describe_status] = describe();
        return description.capabilities;
    }

  public:
    database_on() noexcept = default;
    explicit database_on(receiver_t receiver) noexcept : receiver_(receiver) {}

    /** The C handle of the database, so the receiver passes as a @c ustore_database_t. */
    operator ustore_database_t() const noexcept { return receiver_.database; }

    /** The database and executor this receiver runs on, to build another receiver on them. */
    receiver_t receiver() const noexcept { return receiver_; }

    /** The same head, whose verbs block on @p executor, or on the calling thread without one. */
    database_on<ustore_completion_blocking_k> on(ustore_executor_t executor) const noexcept {
        return database_on<ustore_completion_blocking_k> {receiver_.on(executor)};
    }

    /** The same head, whose verbs submit to @p executor and answer deferred calls. */
    database_on<ustore_completion_deferred_k> deferred(ustore_executor_t executor) const noexcept {
        return database_on<ustore_completion_deferred_k> {receiver_.on(executor)};
    }

    /**
     *  @brief The collection named @p name, refusing one that holds another modality than
     *      @p collection_type_, completing as this receiver does.
     *
     *  @par Statuses:
     *  - @c status_code_t::not_found_k No collection carries @p name.
     *  - @c status_code_t::invalid_argument_k The collection holds another modality than
     *    @p collection_type_.
     */
    template <typename collection_type_ = blobs_collection_t>
    expected<typename collection_type_::template completed_as<completion_>> find(std::string_view name) const noexcept {
        auto [found, find_status] = collection_named(receiver_, name, collection_type_::modality_k);
        if (find_status.failed()) return find_status;
        return typename collection_type_::template completed_as<completion_> {receiver_, found};
    }

    /** The binary collection @p collection identifies, resolved beforehand, so addressing it never
     *  blocks. */
    blobs_collection_on<completion_, collection_access_t::read_write_k> operator[](
        ustore_collection_id_t collection) const noexcept {
        return {receiver_, collection};
    }

    /** Whether a collection named @p name exists at the head. */
    expected<bool> contains(std::string_view name) const noexcept { return collection_exists(receiver_, name); }

#pragma region Verbs

    /** Reports the engine of the database, what it supports and how much it holds. */
    answer<completion_, ustore_database_description_t> describe() const noexcept {
        return called_<completion_, ustore_database_description_t>(
            [&](ustore_outcome_t* outcome, outputs<ustore_database_description_t>* written) noexcept {
                ustore_database_describe_t describe {};
                describe.database = receiver_.database;
                describe.executor = receiver_.executor;
                describe.outcome = outcome;
                describe.completion = completion_;
                describe.description = &written->value;
                ustore_database_describe(&describe);
            });
    }

    /** Lists the collections at the head, with their identifiers in @p arena, borrowed until the
     *  call is final. */
    answer<completion_, collections_list_t> collections(arena_t& arena) const noexcept {
        return collections_list_<completion_>(receiver_, arena);
    }

    /** Creates a collection holding the modality of @p collection_type_ under @p name, borrowed
     *  until the call is final, which no other collection may carry, answering a handle completing
     *  as this receiver does. */
    template <typename collection_type_ = blobs_collection_t>
    answer<completion_, typename collection_type_::template completed_as<completion_>> create(
        std::string_view name) const noexcept {
        using created_t = typename collection_type_::template completed_as<completion_>;
        return called_<completion_, created_t>([&](ustore_outcome_t* outcome, outputs<created_t>* written) noexcept {
            written->value = created_t {receiver_, {}};
            ustore_collection_create_t create {};
            create.database = receiver_.database;
            create.executor = receiver_.executor;
            create.outcome = outcome;
            create.completion = completion_;
            create.name = bytes_of(name);
            create.modality = collection_type_::modality_k;
            create.collection = written->value.member_ptr();
            ustore_collection_create(&create);
        });
    }

    /** Drops what @p mode names of the collection @p collection identifies. */
    answer<completion_, void> drop(ustore_collection_id_t collection,
                                   ustore_drop_mode_t mode = ustore_drop_collection_k) const noexcept {
        return collection_drop_<completion_>(receiver_, collection, mode);
    }

    /**
     *  @brief Empties every collection at once, as @p mode names.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k The RAM and RocksDB engines refuse to drop the
     *    collections while a snapshot is open.
     */
    answer<completion_, void> clear(ustore_drop_mode_t mode) const noexcept {
        return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            ustore_database_clear_t clear {};
            clear.database = receiver_.database;
            clear.executor = receiver_.executor;
            clear.outcome = outcome;
            clear.completion = completion_;
            clear.mode = mode;
            ustore_database_clear(&clear);
        });
    }

    /**
     *  @brief Reclaims the space overwritten and removed data still takes, through its flush,
     *      merge and reclaim phases, reporting its advance to @p progress, which may cancel
     *      it between units.
     *  @param[in] progress Hears every unit, and may cancel the call there, borrowed until the
     *      call is final.
     *  @param[in,out] plan The plan a cancel leaves live, opened by the first call naming it and
     *      continued by every later one, which writes where it stopped; @c nullptr runs the
     *      compaction to its end, and a cancel keeps what it compacted.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k A live @p plan opened on another
     *    database or executor.
     *  - @c status_code_t::read_only_k The database was opened read-only.
     *  - @c status_code_t::unsupported_k The executor computes on a device.
     *  - @c status_code_t::contended_k Another live plan compacts the database.
     */
    answer<completion_, void> compact(progress_arg_t progress = {},
                                      database_compact_plan_t* plan = nullptr) const noexcept {
        return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            ustore_database_compact_t compact {};
            compact.database = receiver_.database;
            compact.executor = receiver_.executor;
            compact.outcome = outcome;
            compact.completion = completion_;
            compact.progress = progress.callback;
            compact.progress_state = progress.state;
            compact.plan = plan ? &plan->handle_ : nullptr;
            compact.phase = plan ? &plan->phase_ : nullptr;
            ustore_database_compact(&compact);
        });
    }

    /**
     *  @brief Closes and frees @p plan, opened on this database and executor, whatever the
     *      call answers, so another compaction may open; a plan a cancel stopped keeps
     *      what it compacted.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k An empty @p plan, or one opened on another
     *    database or executor.
     */
    answer<completion_, void> compact_plan_finish(database_compact_plan_t plan) const noexcept {
        return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            ustore_database_compact_plan_finish_t finish {};
            finish.database = receiver_.database;
            finish.executor = receiver_.executor;
            finish.outcome = outcome;
            finish.completion = completion_;
            finish.plan = plan.handle_;
            ustore_database_compact_plan_finish(&finish);
        });
    }

    /** Makes every commit that returned before this call durable, as if each had asked
     *  for @c ustore_durability_flushed_k. */
    answer<completion_, void> flush() const noexcept {
        return called_<completion_, void>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            ustore_database_flush_t flush {};
            flush.database = receiver_.database;
            flush.executor = receiver_.executor;
            flush.outcome = outcome;
            flush.completion = completion_;
            ustore_database_flush(&flush);
        });
    }

#pragma endregion Verbs

#pragma region Shorthand

    /** Whether the engine runs transactions. */
    bool supports_transactions() const noexcept
        requires blocks_k
    {
        return capabilities_() & ustore_capabilities_transactions_k;
    }

    /** Whether the engine pins snapshots. */
    bool supports_snapshots() const noexcept
        requires blocks_k
    {
        return capabilities_() & ustore_capabilities_snapshots_k;
    }

    /** Begins a transaction, which runs its calls on this receiver's executor. */
    expected<transaction_t> transaction() const noexcept
        requires blocks_k
    {
        return transaction_t::make(receiver_);
    }

    /** Pins a point-in-time view of the database, which the returned snapshot frees
     *  when it goes. */
    expected<snapshot_t> snapshot() const noexcept
        requires blocks_k
    {
        return snapshot_t::make(receiver_);
    }

    /** Finds the collection named @p name, or creates it when absent. */
    expected<blobs_collection_t> operator[](std::string_view name) const noexcept
        requires blocks_k
    {
        return find_or_create(name);
    }

    /** The collection named @p name, created when no collection carries the name. */
    template <typename collection_type_ = blobs_collection_t>
    expected<typename collection_type_::template completed_as<completion_>> find_or_create(
        std::string_view name) const noexcept
        requires blocks_k
    {
        auto found = find<collection_type_>(name);
        if (found || found.code() != status_code_t::not_found_k) return found;
        return create<collection_type_>(name);
    }

    /** Opens a feed of the changes committed to @p collection after @p since, whose pulls run on
     *  this receiver's executor. */
    expected<changes_cursor_t> changes_cursor(ustore_collection_id_t collection,
                                              commit_stamp_t since = {}) const noexcept
        requires blocks_k
    {
        return changes_cursor_t::make(receiver_, collection, since);
    }

    /** Opens an ingest into @p collection, whose rows arrive in @p order and whose calls run on the
     *  calling thread. */
    expected<ingest_t> ingest(ustore_collection_id_t collection,
                              ustore_ingest_order_t order = ustore_ingest_sorted_k) const noexcept
        requires blocks_k
    {
        return ingest_t::make(receiver_.database, collection, order);
    }

    /** Drops the collection @p name addresses, which takes the name with it. */
    status_t drop(std::string_view name) const noexcept
        requires blocks_k
    {
        auto [collection, find_status] = collection_named(receiver_, name);
        if (find_status.failed()) return find_status;
        return drop(collection, ustore_drop_collection_k);
    }

#pragma endregion Shorthand
};

/** The head of a database whose verbs block, as the calls on one executor see it, without owning
 *  either. */
using database_ref_t = database_on<ustore_completion_blocking_k>;

/** A database as a transactional map from collection names to collections, which owns its handle,
 *  and whose verbs run through @c on and @c deferred. It is thread-safe except for the opens and @c
 *  close, never copies, and outlives the collections it hands out, while its transactions and
 *  snapshots may outlive it. */
class database_t {
    ustore_database_t database_ = nullptr;

    /** Opens the engine behind @p init with @p options, blocking on @p executor, whose outputs this
     *  call fills in. */
    template <typename options_type_>
    static expected<database_t> open_(void (*init)(options_type_*), options_type_ options,
                                      ustore_executor_t executor) noexcept {
        database_t database;
        ustore_outcome_t outcome {};
        options.database = &database.database_;
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
    database_t(database_t&& other) noexcept : database_(std::exchange(other.database_, nullptr)) {}
    explicit database_t(ustore_database_t adopted) noexcept : database_(adopted) {}
    database_t& operator=(database_t&& other) noexcept {
        std::swap(database_, other.database_);
        return *this;
    }
    ~database_t() noexcept {
        if (database_) close();
    }

    /** The C handle of the database, so the handle passes as a @c ustore_database_t. */
    operator ustore_database_t() const noexcept { return database_; }

    /** The database with no executor, to build a receiver on it. */
    receiver_t receiver() const noexcept { return receiver_t {database_}; }

    /** The receiver whose verbs block on @p executor, or on the calling thread without one. */
    database_ref_t on(ustore_executor_t executor = nullptr) const noexcept {
        return database_ref_t {receiver_t {database_, executor}};
    }

    /** The receiver whose verbs submit to @p executor and answer deferred calls. */
    database_on<ustore_completion_deferred_k> deferred(ustore_executor_t executor) const noexcept {
        return database_on<ustore_completion_deferred_k> {receiver_t {database_, executor}};
    }

    /** Starts an empty database of the in-memory engine blocking on @p executor, or on the calling
     *  thread without one. The database runs its own calls on the calling thread until a receiver
     *  names an executor. */
    static expected<database_t> init_ram(ustore_database_init_ram_t options = {},
                                         ustore_executor_t executor = nullptr) noexcept {
        return open_(ustore_database_init_ram, options, executor);
    }

    /** Serves the directory @p options name through the in-memory engine, which persists there as
     *  one image, blocking on @p executor, or on the calling thread without one. */
    static expected<database_t> open_ram(ustore_database_open_ram_t options,
                                         ustore_executor_t executor = nullptr) noexcept {
        return open_(ustore_database_open_ram, options, executor);
    }

    /** Serves the directory @p options name through the RocksDB engine, blocking on @p executor, or
     *  on the calling thread without one. */
    static expected<database_t> open_rocksdb(ustore_database_open_rocksdb_t options,
                                             ustore_executor_t executor = nullptr) noexcept {
        return open_(ustore_database_open_rocksdb, options, executor);
    }

    /** Serves the directory @p options name through the RocksDB engine, read-only, blocking on @p
     *  executor, or on the calling thread without one. */
    static expected<database_t> view_rocksdb(ustore_database_view_rocksdb_t options,
                                             ustore_executor_t executor = nullptr) noexcept {
        return open_(ustore_database_view_rocksdb, options, executor);
    }

    /** Serves the directory @p options name through the LevelDB engine, blocking on @p executor, or
     *  on the calling thread without one. */
    static expected<database_t> open_leveldb(ustore_database_open_leveldb_t options,
                                             ustore_executor_t executor = nullptr) noexcept {
        return open_(ustore_database_open_leveldb, options, executor);
    }

    /** Serves the directories @p options name through the NVMe engine on the calling thread,
     *  lending it the executor they name until close. */
    static expected<database_t> open_nvme(ustore_database_open_nvme_t options) noexcept {
        return open_(ustore_database_open_nvme, options, options.executor);
    }

    /** Serves the database the manifest @p options name through the NVMe engine, read-only, writing
     *  nothing to its data directories, blocking on the executor they name or on the caller. */
    static expected<database_t> view_nvme(ustore_database_view_nvme_t options) noexcept {
        return open_(ustore_database_view_nvme, options, options.executor);
    }

    /** Drops this handle, closing the database once no transaction or snapshot of it
     *  holds it open. */
    void close() noexcept { ustore_database_free(std::exchange(database_, nullptr)); }
};

#pragma endregion Databases

} // namespace unum::ustore
