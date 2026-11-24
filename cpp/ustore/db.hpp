/**
 *  @file cpp/ustore/db.hpp
 *  @author Ash Vardanian
 *  @date June 26, 2022
 *  @brief The database, transaction and snapshot handles of the C++ wrappers, the receivers that
 *      run their calls on an executor, blocking or deferred, and the feeds of changes and ingests.
 *
 *  A handle owns its C handle and its lifetime and runs its verbs on the calling thread, @c on
 *  blocks on an executor, both answering a @c status_t or an @c expected, and @c deferred submits
 *  to an executor, answering a @c deferred call. Each verb is written once, as a template on the
 *  completion. Snapshots hand out only read-only collections and proxies, so nothing can write
 *  through a snapshot.
 *
 *  @code{.cpp}
 *  namespace us = unum::ustore;
 *  auto [database, open_status] = us::database_t::init_ram();
 *  auto [executor, executor_status] = us::executor_t::for_ram({.threads = 4});
 *  auto [collection, create_status] = database.create("blobs");
 *  us::deferred<void> written = collection.deferred(executor).write(keys, values);
 *  @endcode
 */
#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <array>       // `std::array`
#include <concepts>    // `std::convertible_to`, `std::same_as`
#include <iterator>    // `std::size`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::forward`, `std::move`

#include "ustore/blobs.hpp"   // `blobs_collection_on`, `blobs_collection_t`, `blobs_ref`, `blobs_read_ref`
#include "ustore/docs.hpp"    // what `find` and `create` take as a template argument
#include "ustore/graph.hpp"   // what `find` and `create` take as a template argument
#include "ustore/pages.hpp"   // what `find` and `create` take as a template argument
#include "ustore/strings.hpp" // what `find` and `create` take as a template argument
#include "ustore/types.hpp"   // `arena_t`, `receiver_t`, `owner`, `opened_`, `answered_`, `progress_t`, `read_stamp_t`

namespace unum::ustore {

#pragma region Lookups

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

/** A handle to one collection of some modality, as a lookup or a creation answers. */
template <typename collection_type_>
concept collection_handle = requires(collection_type_ const& collection) {
    { collection_type_::modality_k } -> std::convertible_to<ustore_modality_t>;
    { collection.handle() } -> std::same_as<ustore_collection_id_t>;
};

/** Where a lookup or a creation writes the collection it answers, beside the receiver the handle
 *  carries. A creation writes the identifier alone, and a lookup the presence, the identifier and
 *  the modality of the one name it was given. */
template <collection_handle collection_type_>
struct outputs<collection_type_> {

    /** The database, executor, and transaction or snapshot the handle carries. */
    receiver_t receiver {};

    /** Output for the identifier a creation minted. */
    ustore_collection_id_t created {};

    /** The two offsets of the one-name tape a lookup reads. */
    std::array<std::uint64_t, 2> name_offsets {};

    /** Outputs for the presence bit, identifier and modality of the name, inside the arena. */
    ustore_octet_t const* presences = nullptr;
    ustore_collection_id_t const* collections = nullptr;
    ustore_modality_t const* modalities = nullptr;

    expected<collection_type_> answer() const noexcept {
        // A creation writes no presences, as its collection exists once the call succeeds
        if (!presences) return {collection_type_ {receiver, created}};
        if (!bits_view_t {presences}[0]) return {{}, status_t {status_code_t::not_found_k, "No such collection"}};
        if (modalities[0] != collection_type_::modality_k)
            return {{}, status_t {status_code_t::invalid_argument_k, "The collection holds another modality"}};
        return {collection_type_ {receiver, collections[0]}};
    }
};

/**
 *  @brief Reads the collection @p name was given at the head, or at the snapshot of @p receiver
 *      when it names one, into @p arena, as @p collection_type_, completing as @p completion_ says.
 *
 *  Collections are created and dropped only at the head, so names are read at the snapshot or at
 *  the head and never join a transaction's conflicts. The caller keeps the bytes of @p name until
 *  the call is final.
 *
 *  @par Statuses:
 *  - @c status_code_t::not_found_k No collection holds @p name.
 *  - @c status_code_t::invalid_argument_k The collection holds another modality than
 *    @p collection_type_.
 */
template <ustore_completion_t completion_, collection_handle collection_type_>
deferrable<collection_type_, completion_> collection_find_(receiver_t const& receiver, std::string_view name,
                                                           arena_t& arena) noexcept {
    return answered_<deferrable<collection_type_, completion_>>(
        [&](ustore_outcome_t* outcome, outputs<collection_type_>* written) noexcept {
            written->receiver = receiver;
            written->name_offsets = {0, name.size()};
            ustore_collection_find_t arguments {.database = receiver.database,
                                                .executor = receiver.executor,
                                                .outcome = outcome,
                                                .completion = completion_,
                                                .snapshot = receiver.snapshot,
                                                .count = 1,
                                                .names = u64_tape(name.data(), written->name_offsets.data()),
                                                .presences = &written->presences,
                                                .collections = &written->collections,
                                                .modalities = &written->modalities,
                                                .arena = arena.handle()};
            ustore_collection_find(&arguments);
        });
}

/** Reads the collection @p name was given, of any modality, through a one-off arena, as the
 *  shorthand that drops or probes a collection by its name does. */
inline expected<ustore_collection_id_t> named_collection_(receiver_t const& receiver, std::string_view name) noexcept {
    auto [arena, arena_status] = arena_t::make({.executor = receiver.executor});
    if (arena_status.failed()) return {{}, arena_status};
    std::array<std::uint64_t, 2> const offsets {0, name.size()};
    ustore_octet_t const* presences = nullptr;
    ustore_collection_id_t const* collections = nullptr;
    ustore_modality_t const* modalities = nullptr;
    ustore_collection_find_t arguments {.database = receiver.database,
                                        .executor = receiver.executor,
                                        .snapshot = receiver.snapshot,
                                        .count = 1,
                                        .names = u64_tape(name.data(), offsets.data()),
                                        .presences = &presences,
                                        .collections = &collections,
                                        .modalities = &modalities,
                                        .arena = arena.handle()};
    status_t const status = answered_<status_t>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        arguments.outcome = outcome;
        ustore_collection_find(&arguments);
    });
    if (status.failed()) return {{}, status};
    if (!bits_view_t {presences}[0]) return {{}, status_t {status_code_t::not_found_k, "No such collection"}};
    return {collections[0]};
}

/** Whether a collection named @p name exists at the head, or at the snapshot of @p receiver. */
inline expected<bool> collection_exists_(receiver_t const& receiver, std::string_view name) noexcept {
    auto [found, lookup_status] = named_collection_(receiver, name);
    if (!lookup_status.failed()) return {true};
    if (lookup_status == status_code_t::not_found_k) return {false};
    return {{}, lookup_status};
}

/** Lists the collections at the head, or at the snapshot of @p receiver, with their identifiers in
 *  @p arena, owned by the caller until the call is final. */
template <ustore_completion_t completion_>
deferrable<collections_list_t, completion_> collections_list_(receiver_t const& receiver, arena_t& arena) noexcept {
    return answered_<deferrable<collections_list_t, completion_>>(
        [&](ustore_outcome_t* outcome, outputs<collections_list_t>* written) noexcept {
            ustore_collection_list_t arguments {.database = receiver.database,
                                                .executor = receiver.executor,
                                                .outcome = outcome,
                                                .completion = completion_,
                                                .snapshot = receiver.snapshot,
                                                .collections = &written->collections,
                                                .written = &written->written,
                                                .arena = arena.handle()};
            ustore_collection_list(&arguments);
        });
}

#pragma endregion Lookups

#pragma region Transactions

/**
 *  @brief A transaction's receiver, which reads and writes through it and completes its verbs as
 *      @p receiver_type_ says.
 *
 *  Batched writes are buffered in the layout the engine prefers until the commit, so a transaction
 *  also saves preprocessing. A transaction serves one call at a time, so a caller waits on one
 *  deferred call before submitting the next.
 */
template <typename receiver_type_>
class transaction {
    static constexpr ustore_completion_t completion_k = receiver_type_::completion;
    template <typename value_type_ = void>
    using deferrable = ustore::deferrable<value_type_, completion_k>;
    receiver_type_ receiver_;

    /** The database the transaction was begun in. */
    ustore_database_t database_ = nullptr;

    template <typename>
    friend class transaction;
    explicit transaction(receiver_type_ receiver, ustore_database_t database) noexcept
        : receiver_(std::move(receiver)), database_(database) {}

  public:
    transaction() noexcept
        requires owns_handle<receiver_type_>
    = default;

    ustore_transaction_t handle() const noexcept { return receiver_.handle(); }

    /** The output field an open writes the new handle into. */
    ustore_transaction_t* handle_field() noexcept
        requires owns_handle<receiver_type_>
    {
        return receiver_.handle_field();
    }

    /** The database, the executor, and the transaction or snapshot every call made here names. */
    receiver_t receiver() const noexcept { return {database_, receiver_.executor(), receiver_.handle(), nullptr}; }

    /** The same handle, whose verbs block on @p executor. */
    transaction<caller_owned<ustore_transaction_t, ustore_completion_blocking_k>> on(
        executor_t const& executor) const noexcept {
        return transaction<caller_owned<ustore_transaction_t, ustore_completion_blocking_k>>(
            {receiver_.handle(), executor.handle()}, database_);
    }

    /** The same handle, whose verbs submit to @p executor and answer deferred calls. */
    transaction<caller_owned<ustore_transaction_t, ustore_completion_deferred_k>> deferred(
        executor_t const& executor) const noexcept {
        return transaction<caller_owned<ustore_transaction_t, ustore_completion_deferred_k>>(
            {receiver_.handle(), executor.handle()}, database_);
    }

    /**
     *  @brief Opens a view of the collection @p name names, following this transaction, read into
     *      @p arena, completing as this receiver does.
     *
     *  @tparam collection_type_ The collection type: @c blobs_collection_t, @c docs_collection_t,
     *      as well as @c graph_collection_t, @c pages_collection_t or @c strings_collection_t.
     */
    template <typename collection_type_ = blobs_collection_t>
    deferrable<typename collection_type_::template completed_as<completion_k>> find(std::string_view name,
                                                                                    arena_t& arena) const noexcept {
        using found_t = typename collection_type_::template completed_as<completion_k>;
        return collection_find_<completion_k, found_t>(receiver(), name, arena);
    }

    /** Shorthand: finds @p name through a one-off arena. */
    template <typename collection_type_ = blobs_collection_t>
    expected<typename collection_type_::template completed_as<completion_k>> find(std::string_view name) const noexcept
        requires blocks<completion_k>
    {
        auto [arena, arena_status] = arena_t::make({.executor = receiver().executor});
        if (arena_status.failed()) return {{}, arena_status};
        return find<collection_type_>(name, arena);
    }

    expected<blobs_collection_on<completion_k, collection_access_t::read_write_k>> operator[](
        std::string_view name) const noexcept
        requires blocks<completion_k>
    {
        return find(name);
    }

    /** Whether a collection named @p name exists at the head. */
    expected<bool> contains(std::string_view name) const noexcept
        requires blocks<completion_k>
    {
        return collection_exists_(receiver(), name);
    }

    blobs_ref<place_batch_t> operator[](strided_span<collection_key_t const> collections_and_keys) const noexcept
        requires blocks<completion_k>
    {
        return {receiver(), locations_places_(collections_and_keys)};
    }

    blobs_ref<place_batch_t> operator[](strided_span<collection_key_field_t const> collections_and_keys) const noexcept
        requires blocks<completion_k>
    {
        return {receiver(), locations_places_(collections_and_keys)};
    }

    template <typename keys_argument_type_>
        requires has_collection<batch_element_of<keys_argument_type_>>
    blobs_ref<keys_argument_type_> operator[](keys_argument_type_&& keys) const noexcept
        requires blocks<completion_k>
    {
        return blobs_ref<keys_argument_type_> {receiver(), std::forward<keys_argument_type_>(keys)};
    }

#pragma region Verbs

    /** Opens the transaction, or clears it for reuse, keeping its memory and starting a new
     *  generation, answering the handle, which reuse keeps. */
    deferrable<ustore_transaction_t> reset() const noexcept {
        return answered_<deferrable<ustore_transaction_t>>(
            [&](ustore_outcome_t* outcome, outputs<ustore_transaction_t>* written) noexcept {
                written->value = receiver().transaction;
                ustore_transaction_init_t arguments {.transaction = &written->value,
                                                     .database = receiver().database,
                                                     .executor = receiver().executor,
                                                     .outcome = outcome,
                                                     .completion = completion_k};
                ustore_transaction_init(&arguments);
            });
    }

    /** Stages the transaction as the first phase of a two-phase commit, answering the stamp it
     *  reads at. */
    deferrable<read_stamp_t> stage() const noexcept {
        return answered_<deferrable<read_stamp_t>>(
            [&](ustore_outcome_t* outcome, outputs<read_stamp_t>* written) noexcept {
                ustore_transaction_stage_t arguments {.database = receiver().database,
                                                      .executor = receiver().executor,
                                                      .outcome = outcome,
                                                      .completion = completion_k,
                                                      .transaction = receiver().transaction,
                                                      .read_stamp = &written->stamp};
                ustore_transaction_stage(&arguments);
            });
    }

    /** Withdraws a stage, releasing what it claimed while keeping the reads and writes, so the
     *  transaction can stage or commit again. */
    deferrable<> rollback() const noexcept {
        return answered_<deferrable<>>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            ustore_transaction_rollback_t arguments {.database = receiver().database,
                                                     .executor = receiver().executor,
                                                     .outcome = outcome,
                                                     .completion = completion_k,
                                                     .transaction = receiver().transaction};
            ustore_transaction_rollback(&arguments);
        });
    }

    /**
     *  @brief Commits every update of the transaction, or none of them if any one fails, answering
     *      the commit's position in commit order.
     *  @param[in] durability Whether the call returns only after the data reaches the disk.
     */
    deferrable<commit_stamp_t> commit(ustore_durability_t durability = ustore_durability_buffered_k) const noexcept {
        return answered_<deferrable<commit_stamp_t>>(
            [&](ustore_outcome_t* outcome, outputs<commit_stamp_t>* written) noexcept {
                ustore_transaction_commit_t arguments {.database = receiver().database,
                                                       .executor = receiver().executor,
                                                       .outcome = outcome,
                                                       .completion = completion_k,
                                                       .transaction = receiver().transaction,
                                                       .commit_stamp = &written->stamp,
                                                       .durability = durability};
                ustore_transaction_commit(&arguments);
            });
    }

#pragma endregion Verbs

    /** Begins a transaction of the database @p receiver names, blocking on the executor it names;
     *  every later verb names its own through @c on or @c deferred. */
    static expected<transaction> make(receiver_t receiver) noexcept
        requires owns_handle<receiver_type_>
    {
        ustore_transaction_init_t arguments {.database = receiver.database, .executor = receiver.executor};
        auto [opened, status] = opened_<transaction>(ustore_transaction_init, arguments,
                                                     &ustore_transaction_init_t::transaction);
        if (status.failed()) return {{}, status};
        opened.database_ = receiver.database;
        return {std::move(opened)};
    }
};

/** A transaction, which owns its handle and serves one call at a time. It never commits on
 *  destruction, never copies, and holds its database open until destroyed, so it may outlive its
 *  @c database_t. Its verbs block on the calling thread, or run through @c on and @c deferred. */
using transaction_t = transaction<owning<ustore_transaction_t, ustore_transaction_free>>;

/** A transaction the caller owns and the executor its verbs run on, completing as
 *  @p completion_ says. */
template <ustore_completion_t completion_>
using transaction_on = transaction<caller_owned<ustore_transaction_t, completion_>>;

/** A transaction whose verbs block, as the calls on one executor see it, without owning either. */
using transaction_ref_t = transaction_on<ustore_completion_blocking_k>;

#pragma endregion Transactions

#pragma region Snapshots

/** A snapshot's receiver, a point-in-time view of the whole database that completes its verbs as @p
 *  receiver_type_ says. It hands out read-only collections and proxies. */
template <typename receiver_type_>
class snapshot {
    static constexpr ustore_completion_t completion_k = receiver_type_::completion;
    template <typename value_type_ = void>
    using deferrable = ustore::deferrable<value_type_, completion_k>;
    receiver_type_ receiver_;

    /** The database the snapshot was pinned in, and the stamp it reads at. */
    ustore_database_t database_ = nullptr;
    read_stamp_t read_stamp_ {};

    template <typename>
    friend class snapshot;
    explicit snapshot(receiver_type_ receiver, ustore_database_t database, read_stamp_t read_stamp) noexcept
        : receiver_(std::move(receiver)), database_(database), read_stamp_(read_stamp) {}

  public:
    snapshot() noexcept
        requires owns_handle<receiver_type_>
    = default;

    ustore_snapshot_t handle() const noexcept { return receiver_.handle(); }

    /** The output field an open writes the new handle into. */
    ustore_snapshot_t* handle_field() noexcept
        requires owns_handle<receiver_type_>
    {
        return receiver_.handle_field();
    }

    /** The database, the executor, and the transaction or snapshot every call made here names. */
    receiver_t receiver() const noexcept { return {database_, receiver_.executor(), nullptr, receiver_.handle()}; }

    /** The stamp the snapshot reads at. */
    read_stamp_t read_stamp() const noexcept { return read_stamp_; }

    /** The same handle, whose verbs block on @p executor. */
    snapshot<caller_owned<ustore_snapshot_t, ustore_completion_blocking_k>> on(
        executor_t const& executor) const noexcept {
        return snapshot<caller_owned<ustore_snapshot_t, ustore_completion_blocking_k>>(
            {receiver_.handle(), executor.handle()}, database_, read_stamp_);
    }

    /** The same handle, whose verbs submit to @p executor and answer deferred calls. */
    snapshot<caller_owned<ustore_snapshot_t, ustore_completion_deferred_k>> deferred(
        executor_t const& executor) const noexcept {
        return snapshot<caller_owned<ustore_snapshot_t, ustore_completion_deferred_k>>(
            {receiver_.handle(), executor.handle()}, database_, read_stamp_);
    }

    /**
     *  @brief Opens a read-only view of the collection @p name names at the snapshot, read into
     *      @p arena, completing as this receiver does.
     *
     *  @tparam collection_type_ The collection type: @c blobs_collection_t, @c docs_collection_t,
     *      as well as @c graph_collection_t, @c pages_collection_t or @c strings_collection_t, or
     *      their read-only twins.
     */
    template <typename collection_type_ = blobs_collection_t>
    deferrable<typename collection_type_::read_only_t::template completed_as<completion_k>> find(
        std::string_view name, arena_t& arena) const noexcept {
        using found_t = typename collection_type_::read_only_t::template completed_as<completion_k>;
        return collection_find_<completion_k, found_t>(receiver(), name, arena);
    }

    /** Shorthand: finds @p name through a one-off arena. */
    template <typename collection_type_ = blobs_collection_t>
    expected<typename collection_type_::read_only_t::template completed_as<completion_k>> find(
        std::string_view name) const noexcept
        requires blocks<completion_k>
    {
        auto [arena, arena_status] = arena_t::make({.executor = receiver().executor});
        if (arena_status.failed()) return {{}, arena_status};
        return find<collection_type_>(name, arena);
    }

    expected<blobs_collection_on<completion_k, collection_access_t::read_only_k>> operator[](
        std::string_view name) const noexcept
        requires blocks<completion_k>
    {
        return find(name);
    }

    /** Whether a collection named @p name exists at the snapshot. */
    expected<bool> contains(std::string_view name) const noexcept
        requires blocks<completion_k>
    {
        return collection_exists_(receiver(), name);
    }

    blobs_read_ref<place_batch_t> operator[](strided_span<collection_key_t const> collections_and_keys) const noexcept
        requires blocks<completion_k>
    {
        return {receiver(), locations_places_(collections_and_keys)};
    }

    blobs_read_ref<place_batch_t> operator[](
        strided_span<collection_key_field_t const> collections_and_keys) const noexcept
        requires blocks<completion_k>
    {
        return {receiver(), locations_places_(collections_and_keys)};
    }

    template <typename keys_argument_type_>
        requires has_collection<batch_element_of<keys_argument_type_>>
    blobs_read_ref<keys_argument_type_> operator[](keys_argument_type_&& keys) const noexcept
        requires blocks<completion_k>
    {
        return blobs_read_ref<keys_argument_type_> {receiver(), std::forward<keys_argument_type_>(keys)};
    }

#pragma region Verbs

    /** Lists the collections at the snapshot, with their identifiers in @p arena, viewed until
     *  the call is final. */
    deferrable<collections_list_t> collections(arena_t& arena) const noexcept {
        return collections_list_<completion_k>(receiver(), arena);
    }

    /** Saves the snapshot as a new database at @p path, owned by the caller until the call is
     *  final, reporting the bytes saved to @p progress, which may cancel the save and so remove
     *  what it wrote. */
    deferrable<> save(std::string_view path, progress_t progress = {}) const noexcept {
        return answered_<deferrable<>>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            ustore_snapshot_save_t arguments {.snapshot = receiver().snapshot,
                                              .executor = receiver().executor,
                                              .outcome = outcome,
                                              .completion = completion_k,
                                              .path = bytes_of(path),
                                              .progress = progress.callback,
                                              .progress_state = progress.state};
            ustore_snapshot_save(&arguments);
        });
    }

#pragma endregion Verbs

    /** Pins a point-in-time view of the database @p receiver names, blocking on the executor it
     *  names; every later verb names its own through @c on or @c deferred. */
    static expected<snapshot> make(receiver_t receiver) noexcept
        requires owns_handle<receiver_type_>
    {
        ustore_stamp_t read_stamp = 0;
        ustore_snapshot_init_t arguments {.database = receiver.database,
                                          .executor = receiver.executor,
                                          .read_stamp = &read_stamp};
        auto [opened, status] = opened_<snapshot>(ustore_snapshot_init, arguments, &ustore_snapshot_init_t::snapshot);
        if (status.failed()) return {{}, status};
        opened.database_ = receiver.database;
        opened.read_stamp_ = read_stamp_t {read_stamp};
        return {std::move(opened)};
    }
};

/** A point-in-time view of the whole database, which reads through it see until its destructor
 *  frees it. It owns its handle, never copies, and holds its database open until destroyed, so it
 *  may outlive its @c database_t. Its verbs block on the calling thread, and @c on and
 *  @c deferred run them on an executor instead. */
using snapshot_t = snapshot<owning<ustore_snapshot_t, ustore_snapshot_free>>;

/** A snapshot the caller owns and the executor its verbs run on, completing as @p completion_
 *  says. */
template <ustore_completion_t completion_>
using snapshot_on = snapshot<caller_owned<ustore_snapshot_t, completion_>>;

/** A snapshot whose verbs block, as the calls on one executor see it, without owning either. */
using snapshot_ref_t = snapshot_on<ustore_completion_blocking_k>;

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

    /** The bytes every change holds, empty for a removal, or none when the pull read none. */
    strings_view_t values {};
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

    owner<ustore_changes_cursor_t, ustore_changes_cursor_free> owner_;

    /** Pulls up to @p wanted changes into @p arena on @p executor, with their bytes into @p values
     *  unless it is null. */
    expected<changes_t> pull_(std::size_t wanted, arena_t& arena, executor_t const& executor,
                              ustore_strings_view_t* values) noexcept {
        ustore_size_t count = 0;
        ustore_key_t const* keys = nullptr;
        ustore_stamp_t const* stamps = nullptr;
        ustore_change_kind_t const* kinds = nullptr;
        ustore_changes_cursor_next_t arguments {.cursor = owner_.handle(),
                                                .executor = executor.handle(),
                                                .wanted = wanted,
                                                .count = &count,
                                                .keys = &keys,
                                                .stamps = &stamps,
                                                .kinds = &kinds,
                                                .values = values,
                                                .arena = arena.handle()};
        status_t const status = answered_<status_t>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            arguments.outcome = outcome;
            ustore_changes_cursor_next(&arguments);
        });
        if (status.failed()) return {{}, status};
        std::size_t const pulled = static_cast<std::size_t>(count);
        changes_t changes {{keys, pulled}, {stamps, pulled}, {kinds, pulled}};
        if (values) changes.values = strings_view_t {*values, pulled};
        return {changes};
    }

  public:
    ustore_changes_cursor_t handle() const noexcept { return owner_.handle(); }

    /** The output field @c make writes the opened feed into. */
    ustore_changes_cursor_t* handle_field() noexcept { return owner_.handle_field(); }

    /**
     *  @brief Opens a feed of the changes committed to @p collection after @p since, in the
     *      database @p receiver names and blocking on its executor, reading nothing until its first
     *      pull, which names its own.
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
        ustore_changes_cursor_init_t arguments {.database = receiver.database,
                                                .executor = receiver.executor,
                                                .collection = collection,
                                                .since_stamp = static_cast<ustore_stamp_t>(since)};
        return opened_<changes_cursor_t>(ustore_changes_cursor_init, arguments, &ustore_changes_cursor_init_t::cursor);
    }

    /** Pulls up to @p wanted next changes into @p arena on @p executor, or on the calling thread
     *  without one, none once the feed is at its newest commit. */
    expected<changes_t> next(std::size_t wanted, arena_t& arena, executor_t const& executor = {}) noexcept {
        return pull_(wanted, arena, executor, nullptr);
    }

    /** Pulls up to @p wanted next changes with the bytes every one holds into @p arena in one
     *  call, on @p executor, or on the calling thread without one. */
    expected<changes_t> next_pairs(std::size_t wanted, arena_t& arena, executor_t const& executor = {}) noexcept {
        ustore_strings_view_t values {};
        return pull_(wanted, arena, executor, &values);
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
 *  Its calls run one at a time, each blocking on the executor it names or on the calling thread
 *  without one, and copy their rows before returning, so every input may die after the call. Its
 *  state lives on the memory domain of the executor @c make names. It moves but never copies.
 */
class ingest_t {
    owner<ustore_ingest_t, ustore_ingest_free> owner_;

  public:
    ustore_ingest_t handle() const noexcept { return owner_.handle(); }

    /** The output field @c make writes the new ingest into. */
    ustore_ingest_t* handle_field() noexcept { return owner_.handle_field(); }

    /**
     *  @brief Opens an ingest into @p collection of the database @p receiver names, whose rows
     *      arrive in @p order, blocking on its executor, whose memory domain holds the ingest.
     *
     *  @par Statuses:
     *  - @c status_code_t::read_only_k The database was opened read-only.
     *  - @c status_code_t::unsupported_k The engine ingests nothing, or not into the collection's
     *    modality.
     *  - @c status_code_t::not_found_k The collection does not exist.
     */
    static expected<ingest_t> make(receiver_t receiver, ustore_collection_id_t collection,
                                   ustore_ingest_order_t order = ustore_ingest_sorted_k) noexcept {
        ustore_ingest_init_t arguments {.database = receiver.database,
                                        .executor = receiver.executor,
                                        .collection = collection,
                                        .order = order};
        return opened_<ingest_t>(ustore_ingest_init, arguments, &ustore_ingest_init_t::ingest);
    }

    /**
     *  @brief Hands the ingest one batch of @p values beside their @p keys, pairwise: the values
     *      of blobs, JSON documents and XML pages, or the strings of a strings collection beside
     *      the keys they map onto, parsed over the threads of @p executor, or on the calling
     *      thread without one.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k The @p keys and @p values differ in count, the ingest
     *    is finished, or a key does not ascend past every key before it under
     *    @c ustore_ingest_sorted_k.
     *  - @c status_code_t::malformed_k A value is past what the collection admits, a document no
     *    JSON, or a page no XML.
     */
    template <strings_batch values_type_>
    status_t append(keys_view_t keys, values_type_ const& values, executor_t const& executor = {}) noexcept {
        // The call has one count, so only this side sees the keys and the values disagree
        if (keys.size() != std::size(values))
            return status_t {status_code_t::invalid_argument_k, "Every key needs its own value"};
        ustore_ingest_append_t arguments {.ingest = handle(),
                                          .executor = executor.handle(),
                                          .count = keys.size(),
                                          .keys = {keys.data(), keys.stride()},
                                          .values = strings_view_of(values)};
        return answered_<status_t>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            arguments.outcome = outcome;
            ustore_ingest_append(&arguments);
        });
    }

    /** Hands the ingest of a graph collection one batch of @p edges on @p executor, or on the
     *  calling thread without one, refusing what the other @c append refuses. */
    status_t append(edges_view_t const& edges, executor_t const& executor = {}) noexcept {
        ustore_ingest_append_t arguments {.ingest = handle(),
                                          .executor = executor.handle(),
                                          .count = edges.size(),
                                          .sources = {edges.sources.data(), edges.sources.stride()},
                                          .targets = {edges.targets.data(), edges.targets.stride()},
                                          .edges = {edges.ids.data(), edges.ids.stride()}};
        return answered_<status_t>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            arguments.outcome = outcome;
            ustore_ingest_append(&arguments);
        });
    }

    /**
     *  @brief Lands every row appended as one commit on @p executor, or on the calling thread
     *      without one, and ends the ingest, whatever it answers, answering the one stamp every
     *      row landed at.
     *
     *  @par Statuses:
     *  - @c status_code_t::invalid_argument_k The ingest is finished.
     *  - @c status_code_t::not_found_k The collection was dropped meanwhile.
     *  - @c status_code_t::contended_k Memory runs kept refilling while the ingest waited them out.
     */
    expected<commit_stamp_t> finish(ustore_durability_t durability = ustore_durability_buffered_k,
                                    executor_t const& executor = {}) noexcept {
        ustore_stamp_t stamp = 0;
        ustore_ingest_finish_t arguments {.ingest = handle(),
                                          .executor = executor.handle(),
                                          .durability = durability,
                                          .commit_stamp = &stamp};
        status_t const status = answered_<status_t>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            arguments.outcome = outcome;
            ustore_ingest_finish(&arguments);
        });
        if (status.failed()) return {{}, status};
        return {commit_stamp_t {stamp}};
    }
};

#pragma endregion Ingests

#pragma region Databases

template <typename receiver_type_>
class database;

/**
 *  @brief A compaction in flight, which @c compact opens on its first call and continues on every
 *      later one, until @c compact_plan_finish consumes it.
 *
 *  Nothing finishes it on destruction: a dropped live plan leaks and keeps its database closed to
 *  another compaction, as an unfinished plan does in C. It opens only on an executor, continues or
 *  finishes on any host executor, and its database outlives every call naming it and its finish.
 */
class database_compact_plan_t {
    ustore_database_compact_plan_t handle_ = nullptr;
    ustore_phase_progress_t phase_ {};

    template <typename>
    friend class database;

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
 *  receiver_type_ says: an owning handle blocks on the calling thread, and one viewing a handle the
 *  caller owns runs on the executor it names. */
template <typename receiver_type_>
class database {
    static constexpr ustore_completion_t completion_k = receiver_type_::completion;
    template <typename value_type_ = void>
    using deferrable = ustore::deferrable<value_type_, completion_k>;
    receiver_type_ receiver_;

    template <typename>
    friend class database;
    explicit database(receiver_type_ receiver) noexcept : receiver_(std::move(receiver)) {}

  public:
    database() noexcept
        requires owns_handle<receiver_type_>
    = default;

    /** Takes over @p adopted, which this handle frees. */
    explicit database(ustore_database_t adopted) noexcept
        requires owns_handle<receiver_type_>
    {
        *receiver_.handle_field() = adopted;
    }

    ustore_database_t handle() const noexcept { return receiver_.handle(); }

    /** The output field an open writes the new handle into. */
    ustore_database_t* handle_field() noexcept
        requires owns_handle<receiver_type_>
    {
        return receiver_.handle_field();
    }

    /** The database, the executor, and the transaction or snapshot every call made here names. */
    receiver_t receiver() const noexcept { return {receiver_.handle(), receiver_.executor()}; }

    /** The same handle, whose verbs block on @p executor. */
    database<caller_owned<ustore_database_t, ustore_completion_blocking_k>> on(
        executor_t const& executor) const noexcept {
        return database<caller_owned<ustore_database_t, ustore_completion_blocking_k>>(
            {receiver_.handle(), executor.handle()});
    }

    /** The same handle, whose verbs submit to @p executor and answer deferred calls. */
    database<caller_owned<ustore_database_t, ustore_completion_deferred_k>> deferred(
        executor_t const& executor) const noexcept {
        return database<caller_owned<ustore_database_t, ustore_completion_deferred_k>>(
            {receiver_.handle(), executor.handle()});
    }

    /**
     *  @brief The collection named @p name, read into @p arena, refusing one that holds another
     *      modality than @p collection_type_, completing as this receiver does.
     *
     *  @par Statuses:
     *  - @c status_code_t::not_found_k No collection holds @p name.
     *  - @c status_code_t::invalid_argument_k The collection holds another modality than
     *    @p collection_type_.
     */
    template <typename collection_type_ = blobs_collection_t>
    deferrable<typename collection_type_::template completed_as<completion_k>> find(std::string_view name,
                                                                                    arena_t& arena) const noexcept {
        using found_t = typename collection_type_::template completed_as<completion_k>;
        return collection_find_<completion_k, found_t>(receiver(), name, arena);
    }

    /** Shorthand: finds @p name through a one-off arena. */
    template <typename collection_type_ = blobs_collection_t>
    expected<typename collection_type_::template completed_as<completion_k>> find(std::string_view name) const noexcept
        requires blocks<completion_k>
    {
        auto [arena, arena_status] = arena_t::make({.executor = receiver().executor});
        if (arena_status.failed()) return {{}, arena_status};
        return find<collection_type_>(name, arena);
    }

    /** The binary collection @p collection identifies, resolved beforehand, so addressing it never
     *  blocks. */
    blobs_collection_on<completion_k, collection_access_t::read_write_k> operator[](
        ustore_collection_id_t collection) const noexcept {
        return {receiver(), collection};
    }

    /** Whether a collection named @p name exists at the head. */
    expected<bool> contains(std::string_view name) const noexcept
        requires blocks<completion_k>
    {
        return collection_exists_(receiver(), name);
    }

#pragma region Verbs

    /** Reports the engine of the database, what it supports and how much it holds. */
    deferrable<ustore_database_description_t> describe() const noexcept {
        return answered_<deferrable<ustore_database_description_t>>(
            [&](ustore_outcome_t* outcome, outputs<ustore_database_description_t>* written) noexcept {
                ustore_database_describe_t arguments {.database = receiver().database,
                                                      .executor = receiver().executor,
                                                      .outcome = outcome,
                                                      .completion = completion_k,
                                                      .description = &written->value};
                ustore_database_describe(&arguments);
            });
    }

    /** Lists the collections at the head, with their identifiers in @p arena, viewed until the
     *  call is final. */
    deferrable<collections_list_t> collections(arena_t& arena) const noexcept {
        return collections_list_<completion_k>(receiver(), arena);
    }

    /** Creates a collection holding the modality of @p collection_type_ under @p name, viewed
     *  until the call is final, which no other collection may hold, answering a handle completing
     *  as this receiver does. */
    template <typename collection_type_ = blobs_collection_t>
    deferrable<typename collection_type_::template completed_as<completion_k>> create(
        std::string_view name) const noexcept {
        using created_t = typename collection_type_::template completed_as<completion_k>;
        return answered_<deferrable<created_t>>([&](ustore_outcome_t* outcome, outputs<created_t>* written) noexcept {
            written->receiver = receiver();
            ustore_collection_create_t arguments {.database = receiver().database,
                                                  .executor = receiver().executor,
                                                  .outcome = outcome,
                                                  .completion = completion_k,
                                                  .name = bytes_of(name),
                                                  .modality = collection_type_::modality_k,
                                                  .collection = &written->created};
            ustore_collection_create(&arguments);
        });
    }

    /**
     *  @brief Drops what @p mode names of the collection @p collection identifies.
     *
     *  @par Statuses:
     *  - @c status_code_t::contended_k Dropping the collection while a snapshot is open, or on NVMe
     *    while a snapshot or a transaction is open.
     */
    deferrable<> drop(ustore_collection_id_t collection,
                      ustore_drop_mode_t mode = ustore_drop_collection_k) const noexcept {
        return collection_drop_<completion_k>(receiver(), collection, mode);
    }

    /**
     *  @brief Empties every collection at once, as @p mode names.
     *
     *  @par Statuses:
     *  - @c status_code_t::contended_k Dropping the collections while a snapshot is open, or on
     *    NVMe while a snapshot or a transaction is open.
     */
    deferrable<> clear(ustore_drop_mode_t mode) const noexcept {
        return answered_<deferrable<>>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            ustore_database_clear_t arguments {.database = receiver().database,
                                               .executor = receiver().executor,
                                               .outcome = outcome,
                                               .completion = completion_k,
                                               .mode = mode};
            ustore_database_clear(&arguments);
        });
    }

    /**
     *  @brief Reclaims the space overwritten and removed data still takes, through its flush,
     *      merge and reclaim phases, reporting its advance to @p progress, which may cancel
     *      it between units.
     *  @param[in] progress Hears every unit, and may cancel the call there, viewed until the
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
    deferrable<> compact(progress_t progress = {}, database_compact_plan_t* plan = nullptr) const noexcept {
        return answered_<deferrable<>>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            ustore_database_compact_t arguments {.database = receiver().database,
                                                 .executor = receiver().executor,
                                                 .outcome = outcome,
                                                 .completion = completion_k,
                                                 .progress = progress.callback,
                                                 .progress_state = progress.state,
                                                 .plan = plan ? &plan->handle_ : nullptr,
                                                 .phase = plan ? &plan->phase_ : nullptr};
            ustore_database_compact(&arguments);
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
    deferrable<> compact_plan_finish(database_compact_plan_t plan) const noexcept {
        return answered_<deferrable<>>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            ustore_database_compact_plan_finish_t arguments {.database = receiver().database,
                                                             .executor = receiver().executor,
                                                             .outcome = outcome,
                                                             .completion = completion_k,
                                                             .plan = plan.handle_};
            ustore_database_compact_plan_finish(&arguments);
        });
    }

    /** Makes every commit that returned before this call durable, as if each had asked
     *  for @c ustore_durability_flushed_k. */
    deferrable<> flush() const noexcept {
        return answered_<deferrable<>>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            ustore_database_flush_t arguments {.database = receiver().database,
                                               .executor = receiver().executor,
                                               .outcome = outcome,
                                               .completion = completion_k};
            ustore_database_flush(&arguments);
        });
    }

#pragma endregion Verbs

#pragma region Shorthand

    /** Whether the engine runs transactions, as its description reports. */
    expected<bool> supports_transactions() const noexcept
        requires blocks<completion_k>
    {
        auto [description, describe_status] = describe();
        if (describe_status.failed()) return {{}, describe_status};
        return {(description.capabilities & ustore_capabilities_transactions_k) != 0};
    }

    /** Whether the engine pins snapshots, as its description reports. */
    expected<bool> supports_snapshots() const noexcept
        requires blocks<completion_k>
    {
        auto [description, describe_status] = describe();
        if (describe_status.failed()) return {{}, describe_status};
        return {(description.capabilities & ustore_capabilities_snapshots_k) != 0};
    }

    /** Begins a transaction, blocking on this receiver's executor; its verbs name their own. */
    expected<transaction_t> transaction() const noexcept
        requires blocks<completion_k>
    {
        return transaction_t::make(receiver());
    }

    /** Pins a point-in-time view of the database, which the returned snapshot frees
     *  when it goes. */
    expected<snapshot_t> snapshot() const noexcept
        requires blocks<completion_k>
    {
        return snapshot_t::make(receiver());
    }

    /** Finds the binary collection named @p name, refusing with @c status_code_t::not_found_k
     *  when no collection carries it. */
    expected<blobs_collection_on<completion_k, collection_access_t::read_write_k>> operator[](
        std::string_view name) const noexcept
        requires blocks<completion_k>
    {
        return find(name);
    }

    /** The collection named @p name, created when no collection holds the name. */
    template <typename collection_type_ = blobs_collection_t>
    expected<typename collection_type_::template completed_as<completion_k>> find_or_create(
        std::string_view name) const noexcept
        requires blocks<completion_k>
    {
        auto [found, status] = find<collection_type_>(name);
        if (status.code == status_code_t::not_found_k) return create<collection_type_>(name);
        if (status.failed()) return {{}, status};
        return {std::move(found)};
    }

    /** Opens a feed of the changes committed to @p collection after @p since, blocking on this
     *  receiver's executor; its pulls name their own. */
    expected<changes_cursor_t> changes_cursor(ustore_collection_id_t collection,
                                              commit_stamp_t since = {}) const noexcept
        requires blocks<completion_k>
    {
        return changes_cursor_t::make(receiver(), collection, since);
    }

    /** Opens an ingest into @p collection, whose rows arrive in @p order, blocking on
     *  this receiver's executor, whose memory domain holds the ingest; its later calls
     *  name their own. */
    expected<ingest_t> ingest(ustore_collection_id_t collection,
                              ustore_ingest_order_t order = ustore_ingest_sorted_k) const noexcept
        requires blocks<completion_k>
    {
        return ingest_t::make(receiver(), collection, order);
    }

    /** Drops the collection @p name addresses, which takes the name with it. */
    status_t drop(std::string_view name) const noexcept
        requires blocks<completion_k>
    {
        auto [collection, find_status] = named_collection_(receiver(), name);
        if (find_status.failed()) return find_status;
        return drop(collection, ustore_drop_collection_k);
    }

#pragma endregion Shorthand

    /** Starts an empty database of the in-memory engine blocking on @p executor, or on the calling
     *  thread without one. The database runs its own calls on the calling thread until a receiver
     *  names an executor. */
    static expected<database> init_ram(ustore_database_init_ram_t options = {},
                                       executor_t const& executor = {}) noexcept
        requires owns_handle<receiver_type_>
    {
        return opened_<database>(ustore_database_init_ram, options, &ustore_database_init_ram_t::database, executor);
    }

    /** Serves the directory @p options name through the in-memory engine, which persists there as
     *  one image, blocking on @p executor, or on the calling thread without one. */
    static expected<database> open_ram(ustore_database_open_ram_t options, executor_t const& executor = {}) noexcept
        requires owns_handle<receiver_type_>
    {
        return opened_<database>(ustore_database_open_ram, options, &ustore_database_open_ram_t::database, executor);
    }

    /** Serves the directory @p options name through the RocksDB engine, blocking on @p executor, or
     *  on the calling thread without one. */
    static expected<database> open_rocksdb(ustore_database_open_rocksdb_t options,
                                           executor_t const& executor = {}) noexcept
        requires owns_handle<receiver_type_>
    {
        return opened_<database>(ustore_database_open_rocksdb, options, &ustore_database_open_rocksdb_t::database,
                                 executor);
    }

    /** Serves the directory @p options name through the RocksDB engine, read-only, blocking on @p
     *  executor, or on the calling thread without one. */
    static expected<database> view_rocksdb(ustore_database_view_rocksdb_t options,
                                           executor_t const& executor = {}) noexcept
        requires owns_handle<receiver_type_>
    {
        return opened_<database>(ustore_database_view_rocksdb, options, &ustore_database_view_rocksdb_t::database,
                                 executor);
    }

    /** Serves the directory @p options name through the LevelDB engine, blocking on @p executor, or
     *  on the calling thread without one. */
    static expected<database> open_leveldb(ustore_database_open_leveldb_t options,
                                           executor_t const& executor = {}) noexcept
        requires owns_handle<receiver_type_>
    {
        return opened_<database>(ustore_database_open_leveldb, options, &ustore_database_open_leveldb_t::database,
                                 executor);
    }

    /** Serves the directories @p options name through the NVMe engine, moving bytes as an NVMe
     *  executor's placement names, blocking on @p executor or on the calling thread without it. */
    static expected<database> open_nvme(ustore_database_open_nvme_t options, executor_t const& executor = {}) noexcept
        requires owns_handle<receiver_type_>
    {
        return opened_<database>(ustore_database_open_nvme, options, &ustore_database_open_nvme_t::database, executor);
    }

    /** Serves the database the manifest @p options name through the NVMe engine, read-only,
     *  writing nothing to its data directories, blocking on @p executor or on the calling
     *  thread without it. */
    static expected<database> view_nvme(ustore_database_view_nvme_t options, executor_t const& executor = {}) noexcept
        requires owns_handle<receiver_type_>
    {
        return opened_<database>(ustore_database_view_nvme, options, &ustore_database_view_nvme_t::database, executor);
    }

    /** Drops this handle, closing the database once no transaction or snapshot of it holds it
     *  open; closing writes nothing, so @c flush first makes buffered commits durable. */
    void close() noexcept
        requires owns_handle<receiver_type_>
    {
        receiver_ = receiver_type_ {};
    }
};

/** A database as a transactional map from collection names to collections, which owns its handle,
 *  and whose verbs block on the calling thread, or run through @c on and @c deferred. It is
 *  thread-safe except for the opens and @c close, never copies, and outlives the collections it
 *  hands out, while the transactions and snapshots it begins may outlive the database itself. */
using database_t = database<owning<ustore_database_t, ustore_database_free>>;

/** A database head the caller owns and the executor its verbs run on, completing as
 *  @p completion_ says. */
template <ustore_completion_t completion_>
using database_on = database<caller_owned<ustore_database_t, completion_>>;

/** The head of a database whose verbs block, as the calls on one executor see it, without owning
 *  either. */
using database_ref_t = database_on<ustore_completion_blocking_k>;

#pragma endregion Databases

} // namespace unum::ustore
