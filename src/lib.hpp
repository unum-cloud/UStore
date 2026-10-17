/**
 *  @file src/lib.hpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief The router every exported call of the C ABI goes through, to the engine behind its database or transaction
 *      handle, shared by the one source per public header.
 *
 *  Each engine defines only its own `ustore_database_init_*`, `ustore_database_open_*` and `ustore_database_view_*`,
 *  which stamp every handle they hand out with its @c engine_t, so one library holds every engine it was built with
 *  and a program links only those it opens. The opens of an engine the build lacks are defined in `src/db.cpp` and
 *  refuse, so every program links against every build, and every other one hands its engine's open to @c route_init,
 *  which routes it as any call. Every call of the table is `noexcept`, so nothing here catches. The router claims the
 *  arena and the transaction of a call, refuses a change to a database opened read-only, then runs the call on the
 *  calling thread, blocking on its executor, or copies it into the executor's deferred table, and finishes its outcome
 *  only after the engine returns.
 *
 *  A call keeps whatever it needs only while it runs in a scratch, so its arena holds only its outputs. Each pool
 *  thread of an executor and its blocking caller keep one scratch, rewound before every call and kept at its
 *  high-water mark until the executor is freed, and a call without an executor makes a scratch of its own on the
 *  calling thread, starting from one page of its stack, and frees whatever it grew into once final. The router
 *  gathers there the views every accessor of a ragged input writes, on the thread that runs the call, before the
 *  engine sees it, so every engine reads tapes and accessors alike.
 *
 *  An executor owns a pool, spawned or lent, and one driver thread, which hands runs of consecutive deferred calls to
 *  the pool with a blocking dispatch, and runs a batch itself when a lent pool refuses the dispatch while it runs its
 *  lender's. One compare-and-swap on @c submitted takes a call's ticket and, for a deferred call, its table position,
 *  so their orders agree, and every ticket below @c finished is final. A blocking call takes a ticket and no slot, and
 *  runs on the calling thread once every ticket before it is final. The driver frees a batch's table positions before
 *  it finishes any of the batch's outcomes, so a caller that sees a call final may submit the next one at once.
 *
 *  A deferred call raises the deferred count of its database, of the transaction, snapshot or cursor it names, and of
 *  its arena when it takes its slot, and lowers them once its outcome is final, so freeing any of those handles waits
 *  for zero.
 *
 *  Cursors live here, with no engine slot of their own: a pull lists the next page through the engine's scan and reads
 *  its values through the engine's read in one call. A cursor holds its database, and the transaction or snapshot it
 *  reads through, as a transaction holds its database, so each closes with the last of its holds.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`, `std::uintptr_t`
#include <cstring> // `std::memcpy`

#include <algorithm>   // `std::min`
#include <array>       // `std::array`
#include <atomic>      // `std::atomic_ref`, `std::memory_order_release`
#include <bit>         // `std::bit_cast`
#include <new>         // `std::nothrow`, placement `new`
#include <numeric>     // `std::iota`
#include <string_view> // `std::string_view`
#include <thread>      // `std::this_thread::get_id`, `std::thread::id`
#include <tuple>       // `std::apply`
#include <type_traits> // `std::underlying_type_t`
#include <variant>     // `std::variant`, `std::visit`, `std::get_if`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`
#include <stringzilla/stringzilla.hpp> // `sz::string_t`

#include "ustore/types.hpp"                 // `key_after`, `largest_key_k`
#include "ustore/utilities/ram_buffers.hpp" // `arena_rewind`, `arena_copy`, `arena_page_bytes_k`, `export_strings`
#include "ustore/utilities/ram_device.hpp"  // `executor_t`, `lent_pool_t`, `topology_t`, `claim_t`, `residency_t`

#include "engine.hpp"             // `engine_t`, `handle_t`, `view_t`
#include "utilities/batches.hpp"  // `strings_arg_t`
#include "utilities/progress.hpp" // `progress_t`, `progress_cancelled_k`

namespace unum::ustore {
using namespace unum::ustore::utilities;

/** What every cursor holds: the claim of the pull filling it, its deferred pulls, and what it walks. */
struct cursor_t {

    /** Whether a pull fills the cursor, so a second pull naming it finds it contended. */
    claim_t claim = claim_t::free_k;

    /** Deferred pulls naming the cursor that are not final, through `std::atomic_ref`. */
    std::uint32_t deferred_calls = 0;

    /** Whether a pull came back short of what it wanted, so every later one reads nothing. */
    bool exhausted = false;

    /** The database the walk reads, held open until the cursor is freed. */
    ustore_database_t database = nullptr;

    /** What the walk sees, whose transaction or snapshot the cursor holds until freed. */
    ustore_view_t view = nullptr;

    /** The collection the walk reads. */
    ustore_collection_id_t collection {};
};

/** A walk over the keys of one collection, resumed at the key after the last one it handed out. */
struct blobs_cursor_t : cursor_t {

    /** The smallest key the next pull may hand out. */
    ustore_key_t start_key {};
};

/** A walk over the strings of one prefix, resumed at the string after the last one it handed out. */
struct strings_cursor_t : cursor_t {

    /** The prefix every string the walk hands out starts with. */
    sz::string_t prefix;

    /** The smallest string the next pull may hand out. */
    sz::string_t start;
};

#pragma region Names

/** One enumerator of a closed set and its canonical spelling. */
template <typename enum_type_>
struct enumerator_spelling {
    enum_type_ value;
    std::string_view name;
};

/** The spelling of @p value in @p names, or "unrecognized" for a value no enumerator carries. */
template <typename enum_type_, std::size_t count_>
char const* name_in(enumerator_spelling<enum_type_> const (&names)[count_], enum_type_ const& value) noexcept {
    // A C caller may pass any integer, which C++ may not load as the enum, so its bits are read in place instead
    using bits_t = std::underlying_type_t<enum_type_>;
    for (enumerator_spelling<enum_type_> const& spelling : names)
        if (std::bit_cast<bits_t>(spelling.value) == std::bit_cast<bits_t>(value)) return spelling.name.data();
    return "unrecognized";
}

/** The enumerator of @p names spelled @p name, or the zero enumerator for a name none carries. */
template <typename enum_type_, std::size_t count_>
enum_type_ value_in(enumerator_spelling<enum_type_> const (&names)[count_], ustore_bytes_view_t name) noexcept {
    std::string_view const text {reinterpret_cast<char const*>(name.data), name.length};
    for (enumerator_spelling<enum_type_> const& spelling : names)
        if (spelling.name == text) return spelling.value;
    return enum_type_ {};
}

#pragma endregion Names

/** The tag `ustore_transaction_view` and `ustore_snapshot_view` set in the low bits of a handle. */
enum class view_tag_t : std::uintptr_t {
    head_k = 0,
    watching_k = 1,
    blind_k = 2,
    snapshot_k = 3,
};

/** The low bits of a handle a view tag takes, which every handle's alignment leaves free. */
constexpr std::uintptr_t view_tag_mask_k = 3;
static_assert(alignof(handle_t) > view_tag_mask_k);

/** The view naming @p handle with @p tag, or the head for a null @p handle. */
inline ustore_view_t tagged_view(void* handle, view_tag_t tag) noexcept {
    if (!handle) return nullptr;
    return reinterpret_cast<ustore_view_t>(reinterpret_cast<std::uintptr_t>(handle) | static_cast<std::uintptr_t>(tag));
}

/** Decodes @p view into @p decoded, answering false for a pointer neither view constructor made. */
inline bool decode_view(ustore_view_t view, view_t& decoded) noexcept {
    std::uintptr_t const bits = reinterpret_cast<std::uintptr_t>(view);
    void* const handle = reinterpret_cast<void*>(bits & ~view_tag_mask_k);
    switch (static_cast<view_tag_t>(bits & view_tag_mask_k)) {
    case view_tag_t::head_k: return handle == nullptr;
    case view_tag_t::watching_k: decoded.transaction = static_cast<ustore_transaction_t>(handle); return true;
    case view_tag_t::blind_k:
        decoded.transaction = static_cast<ustore_transaction_t>(handle);
        decoded.watch = ustore_watch_none_k;
        return true;
    case view_tag_t::snapshot_k:
        decoded.snapshot = static_cast<ustore_snapshot_t>(handle);
        decoded.watch = ustore_watch_none_k;
        return true;
    }
    return false;
}

/** The transaction a call carries, or null. */
inline ustore_transaction_t carried(ustore_transaction_t transaction) noexcept { return transaction; }

/** The transaction a call reuses, or null for one it opens afresh. */
inline ustore_transaction_t carried(ustore_transaction_t const* transaction) noexcept {
    return transaction ? *transaction : nullptr;
}

/** The snapshot a call reads, or null, which an output for a new snapshot never matches. */
inline ustore_snapshot_t viewed(ustore_snapshot_t snapshot) noexcept { return snapshot; }

/** Decodes what @p arguments read through into @p view, answering false for a view neither view constructor made. A
 *  pull reads through the view its cursor opened with. */
template <typename arguments_type_>
bool decode_call(arguments_type_ const& arguments, view_t& view) noexcept {
    if constexpr (requires { arguments.cursor->view; }) return decode_view(arguments.cursor->view, view);
    else {
        if constexpr (requires { arguments.view; })
            if (!decode_view(arguments.view, view)) return false;
        if constexpr (requires { carried(arguments.transaction); }) view.transaction = carried(arguments.transaction);
        if constexpr (requires { arguments.watch; }) view.watch = arguments.watch;
        if constexpr (requires { viewed(arguments.snapshot); }) view.snapshot = viewed(arguments.snapshot);
        return true;
    }
}

/** Takes one more hold on @p handle, a database, a transaction or a snapshot, so it outlives the caller's free. */
inline void take_hold(void* handle) noexcept {
    std::atomic_ref<std::uint32_t> {handle_cast<handle_t>(handle)->holds}.fetch_add(1, std::memory_order_relaxed);
}

/** Drops one hold on @p handle, answering whether it was the last. */
inline bool drop_hold(void* handle) noexcept {
    std::atomic_ref<std::uint32_t> const holds {handle_cast<handle_t>(handle)->holds};
    return holds.fetch_sub(1, std::memory_order_acq_rel) == 1;
}

/** Keeps @p database open until @p opened, a transaction or a snapshot of it, is freed. */
inline void hold(ustore_database_t database, void* opened) noexcept {
    handle_cast<handle_t>(opened)->database = database;
    take_hold(database);
}

/** Drops one hold on @p database, if any, closing it through its engine with the last. */
inline void release_hold(ustore_database_t database) noexcept {
    if (database && drop_hold(database)) engine_of(database).database_free(database);
}

/** Drops one hold on the transaction or snapshot @p view names, if any, freeing it through its engine with the last,
 *  which gives back the hold it kept on its database. */
inline void release_parent(view_t const& view) noexcept {
    void* const parent = view.transaction ? static_cast<void*>(view.transaction) : view.snapshot;
    if (!parent || !drop_hold(parent)) return;
    ustore_database_t const database = handle_cast<handle_t>(parent)->database;
    if (view.transaction) engine_of(parent).transaction_free(view.transaction);
    else engine_of(parent).snapshot_free(view.snapshot);
    release_hold(database);
}

/** Whether @p type_ is one of @p types_. */
template <typename type_, typename... types_>
constexpr bool one_of_k = (std::is_same_v<type_, types_> || ...);

/** Whether a call changes its database, which one opened read-only refuses: every call naming a transaction, as reads
 *  name views instead, and the calls on whole collections. */
template <typename arguments_type_>
constexpr bool changes_database_k = requires(arguments_type_ const& arguments) { arguments.transaction; } ||
                                    one_of_k<arguments_type_, ustore_collection_create_t, ustore_collection_drop_t,
                                             ustore_database_clear_t, ustore_database_compact_t>;

#pragma region Outputs

/** Leaves the view, handle or scalar @p output points to null or zero, when the caller asked for it. */
template <typename output_type_>
void clear(output_type_* output) noexcept {
    if (output) *output = {};
}

/** Leaves every output of a call null or zero, as a call that fails or has nothing to report does. */
inline void clear_outputs(ustore_database_describe_t const& arguments) noexcept { clear(arguments.description); }

inline void clear_outputs(ustore_snapshot_init_t const& arguments) noexcept {
    clear(arguments.snapshot), clear(arguments.read_stamp);
}

inline void clear_outputs(ustore_collection_list_t const& arguments) noexcept {
    clear(arguments.collections), clear(arguments.modalities), clear(arguments.names), clear(arguments.written);
}

inline void clear_outputs(ustore_collection_create_t const& arguments) noexcept { clear(arguments.collection); }

inline void clear_outputs(ustore_collection_find_t const& arguments) noexcept {
    clear(arguments.presences), clear(arguments.collections), clear(arguments.modalities);
}

inline void clear_outputs(ustore_transaction_stage_t const& arguments) noexcept { clear(arguments.read_stamp); }

inline void clear_outputs(ustore_transaction_commit_t const& arguments) noexcept { clear(arguments.commit_stamp); }

inline void clear_outputs(ustore_blobs_read_t const& arguments) noexcept {
    clear(arguments.presences), clear(arguments.lengths), clear(arguments.values);
}

inline void clear_outputs(ustore_blobs_scan_t const& arguments) noexcept {
    clear(arguments.offsets), clear(arguments.keys);
}

inline void clear_outputs(ustore_blobs_sample_t const& arguments) noexcept {
    clear(arguments.offsets), clear(arguments.keys);
}

inline void clear_outputs(ustore_blobs_measure_t const& arguments) noexcept {
    clear(arguments.min_cardinalities), clear(arguments.max_cardinalities), clear(arguments.min_value_bytes);
    clear(arguments.max_value_bytes), clear(arguments.min_space_usages), clear(arguments.max_space_usages);
}

inline void clear_outputs(ustore_docs_read_t const& arguments) noexcept {
    clear(arguments.presences), clear(arguments.lengths), clear(arguments.values);
}

inline void clear_outputs(ustore_docs_gist_t const& arguments) noexcept {
    clear(arguments.fields), clear(arguments.written);
}

inline void clear_outputs(ustore_docs_gather_t const& arguments) noexcept { clear(arguments.columns); }

inline void clear_outputs(ustore_graph_read_edges_t const& arguments) noexcept {
    clear(arguments.degrees_per_vertex), clear(arguments.edges_offsets), clear(arguments.edges_per_vertex);
}

inline void clear_outputs(ustore_pages_insert_t const& arguments) noexcept { clear(arguments.effective_keys); }

inline void clear_outputs(ustore_pages_read_t const& arguments) noexcept {
    clear(arguments.presences), clear(arguments.lengths), clear(arguments.values);
}

inline void clear_outputs(ustore_pages_history_t const& arguments) noexcept {
    clear(arguments.edits_offsets), clear(arguments.stamps), clear(arguments.modifications), clear(arguments.elements);
    clear(arguments.parents), clear(arguments.anchors), clear(arguments.fields), clear(arguments.text_starts);
    clear(arguments.text_lengths), clear(arguments.values);
}

inline void clear_outputs(ustore_strings_read_t const& arguments) noexcept {
    clear(arguments.presences), clear(arguments.lengths), clear(arguments.values);
}

inline void clear_outputs(ustore_strings_scan_t const& arguments) noexcept {
    clear(arguments.offsets), clear(arguments.strings);
}

inline void clear_outputs(ustore_blobs_cursor_next_t const& arguments) noexcept {
    clear(arguments.keys), clear(arguments.values), clear(arguments.written);
}

inline void clear_outputs(ustore_strings_cursor_next_t const& arguments) noexcept {
    clear(arguments.strings), clear(arguments.values), clear(arguments.written);
}

#pragma endregion Outputs

#pragma region Ragged Inputs

/** A ragged input of a call and the number of strings it holds. */
struct ragged_input_t {
    ustore_strings_view_t view;
    std::size_t count;
};

/** The ragged inputs of every call that takes them, in the order its engine slot reads them. */
inline std::array<ragged_input_t, 1> ragged_inputs(ustore_collection_find_t const& arguments) noexcept {
    return {{{arguments.names, arguments.count}}};
}

inline std::array<ragged_input_t, 1> ragged_inputs(ustore_blobs_write_t const& arguments) noexcept {
    return {{{arguments.values, arguments.count}}};
}

inline std::array<ragged_input_t, 2> ragged_inputs(ustore_docs_write_t const& arguments) noexcept {
    return {{{arguments.fields, arguments.count}, {arguments.values, arguments.count}}};
}

inline std::array<ragged_input_t, 1> ragged_inputs(ustore_docs_read_t const& arguments) noexcept {
    return {{{arguments.fields, arguments.count}}};
}

inline std::array<ragged_input_t, 1> ragged_inputs(ustore_docs_gather_t const& arguments) noexcept {
    return {{{arguments.fields, arguments.fields_count}}};
}

inline std::array<ragged_input_t, 1> ragged_inputs(ustore_pages_import_t const& arguments) noexcept {
    return {{{arguments.values, arguments.count}}};
}

inline std::array<ragged_input_t, 1> ragged_inputs(ustore_pages_insert_t const& arguments) noexcept {
    return {{{arguments.values, arguments.count}}};
}

inline std::array<ragged_input_t, 1> ragged_inputs(ustore_pages_splice_t const& arguments) noexcept {
    return {{{arguments.values, arguments.count}}};
}

inline std::array<ragged_input_t, 2> ragged_inputs(ustore_pages_set_attributes_t const& arguments) noexcept {
    return {{{arguments.fields, arguments.count}, {arguments.values, arguments.count}}};
}

inline std::array<ragged_input_t, 1> ragged_inputs(ustore_pages_remove_attributes_t const& arguments) noexcept {
    return {{{arguments.fields, arguments.count}}};
}

inline std::array<ragged_input_t, 2> ragged_inputs(ustore_strings_write_t const& arguments) noexcept {
    return {{{arguments.strings, arguments.count}, {arguments.values, arguments.count}}};
}

inline std::array<ragged_input_t, 1> ragged_inputs(ustore_strings_remove_t const& arguments) noexcept {
    return {{{arguments.strings, arguments.count}}};
}

inline std::array<ragged_input_t, 1> ragged_inputs(ustore_strings_read_t const& arguments) noexcept {
    return {{{arguments.strings, arguments.count}}};
}

inline std::array<ragged_input_t, 2> ragged_inputs(ustore_strings_scan_t const& arguments) noexcept {
    return {{{arguments.prefixes, arguments.count}, {arguments.start_strings, arguments.count}}};
}

/** Strings an accessor writes per block, so the indices it reads fit on the stack of the thread running the call. */
constexpr std::size_t accessor_block_k = 64;

/** Reads @p input for its engine into @p strings, refusing a layout no constant names and gathering into @p scratch
 *  the views an accessor writes, so the accessor runs on the thread running the call and never after it is final. */
inline status_t gather_strings(ragged_input_t const& input, ustore_arena_t scratch, strings_arg_t& strings) noexcept {
    ustore_strings_view_t const& view = input.view;
    strings.view = view;
    // A C caller may pass any integer, which C++ may not load as the enum, so its bits are read instead
    switch (std::bit_cast<std::underlying_type_t<ustore_strings_layout_t>>(view.layout)) {
    case ustore_strings_u32_tape_k: return {};
    case ustore_strings_u64_tape_k:
        if (input.count && !view.data.u64_tape.offsets)
            return {status_code_t::invalid_argument_k, "A tape of strings needs its offsets"};
        return {};
    case ustore_strings_accessor_k: break;
    default: return {status_code_t::invalid_argument_k, "The strings come in an unknown layout"};
    }
    if (!input.count) return {};
    if (!view.data.accessor.get) return {status_code_t::invalid_argument_k, "An accessor of strings needs its get"};

    auto [views, status] = arena_allocate<ustore_bytes_view_t>(scratch, input.count);
    if (status.failed()) return status;
    std::array<ustore_size_t, accessor_block_k> indices;
    for (std::size_t first = 0; first < input.count; first += indices.size()) {
        std::size_t const block = std::min(indices.size(), input.count - first);
        std::iota(indices.begin(), indices.begin() + block, static_cast<ustore_size_t>(first));
        view.data.accessor.get(view.data.accessor.handle, indices.data(), block, views.data() + first);
    }
    strings.gathered = views.data();
    return {};
}

#pragma endregion Ragged Inputs

#pragma region Cursors

/**
 *  @brief Pulls the next page of the walk @p arguments name through @p engine, as one call over the arena they give.
 *
 *  The engine's scan lists the keys into the scratch of @p view, its read writes their values into the arena when
 *  asked, and the keys follow them there, so the arena holds only the outputs. The cursor advances only once the
 *  progress callback let the pull finish.
 */
status_t pull(engine_t const& engine, ustore_blobs_cursor_next_t const& arguments, blobs_cursor_t& cursor,
              view_t const& view) noexcept;

/** Pulls the next page of the walk @p arguments name through @p engine, as the blobs pull does, resuming past the last
 *  string at that string followed by a zero byte, the smallest string after it. */
status_t pull(engine_t const& engine, ustore_strings_cursor_next_t const& arguments, strings_cursor_t& cursor,
              view_t const& view) noexcept;

#pragma endregion Cursors

/** Serves @p arguments on the engine of their database in @p scratch through @p slot_, refusing what no engine may
 *  see, and handing every ragged input to the engine readable in any layout. A pull serves through its cursor, whose
 *  walk @p slot_ lists. */
template <auto slot_, typename arguments_type_>
status_t serve(arguments_type_ const& arguments, ustore_arena_t scratch) noexcept {
    ustore_database_t const database = database_of(arguments);
    if (!database) return {status_code_t::uninitialized_k, "The database is uninitialized"};
    engine_t const& engine = engine_of(database);
    if (!(engine.*slot_)) return {status_code_t::unsupported_k, "The engine does not implement this call"};
    if constexpr (changes_database_k<arguments_type_>)
        if (handle_cast<handle_t>(database)->access == access_t::read_only_k)
            return {status_code_t::read_only_k, "The database was opened read-only"};
    if constexpr (requires { arguments.cursor; })
        if (arguments.cursor->database != database)
            return {status_code_t::invalid_argument_k, "The cursor walks another database"};

    view_t view {.scratch = scratch};
    if (!decode_call(arguments, view))
        return {status_code_t::invalid_argument_k, "The view was made by neither view constructor"};
    void* const handle = view.transaction ? static_cast<void*>(view.transaction) : view.snapshot;
    if (handle && &engine_of(handle) != &engine)
        return {status_code_t::invalid_argument_k, "The view belongs to another engine"};

    if constexpr (requires { arguments.arena; }) arena_rewind(arguments.arena);
    arena_rewind(scratch);
    if constexpr (std::is_same_v<arguments_type_, ustore_transaction_init_t>) {
        ustore_transaction_t const reused = arguments.transaction ? *arguments.transaction : nullptr;
        if (reused && handle_cast<handle_t>(reused)->database != arguments.database)
            return {status_code_t::invalid_argument_k, "The transaction belongs to another database"};
        status_t const status = (engine.*slot_)(arguments, view);
        if (!status.failed() && !reused) hold(arguments.database, *arguments.transaction);
        return status;
    }
    else if constexpr (std::is_same_v<arguments_type_, ustore_snapshot_init_t>) {
        status_t const status = (engine.*slot_)(arguments, view);
        if (!status.failed()) hold(arguments.database, *arguments.snapshot);
        return status;
    }
    else if constexpr (requires { arguments.cursor; }) return pull(engine, arguments, *arguments.cursor, view);
    else if constexpr (requires { ragged_inputs(arguments); }) {
        auto const inputs = ragged_inputs(arguments);
        std::array<strings_arg_t, std::tuple_size_v<decltype(inputs)>> strings;
        for (std::size_t index = 0; index != inputs.size(); ++index)
            if (status_t const status = gather_strings(inputs[index], scratch, strings[index]); status.failed())
                return status;
        return std::apply(
            [&](auto const&... gathered) noexcept { return (engine.*slot_)(arguments, gathered..., view); }, strings);
    }
    else return (engine.*slot_)(arguments, view);
}

/** Finishes a call: the message, then the status with release ordering, then a wake for whoever waits on it. */
inline void finish(ustore_outcome_t& outcome, status_t status) noexcept {
    outcome.message = status.message;
    std::atomic_ref<ustore_status_t> const word {outcome.status};
    word.store(static_cast<ustore_status_t>(status.code), std::memory_order_release);
    word.notify_all();
}

#pragma region Executors

/** Deferred calls an executor holds at once when its constructor names no limit. */
constexpr std::size_t deferred_limit_default_k = 256;

/** The most deferred calls an executor holds, keeping every unfinished ticket within half the range of its counter. */
constexpr std::size_t deferred_limit_max_k = std::size_t {1} << 30;

/** Microseconds a parked pool sleeps between checks for the next batch. */
constexpr std::size_t parked_check_micros_k = 1000;

/** The init of an engine with the open serving it, which the router runs as it runs every call. */
template <typename arguments_type_>
struct routed_init : arguments_type_ {
    status_t (*open)(arguments_type_ const&, view_t const&) noexcept = nullptr;
};

/** Every argument struct that names an executor, and every routed init with its open, so a deferred submission keeps
 *  its call by value. */
using deferred_arguments_t = std::variant<
    ustore_database_describe_t, ustore_database_clear_t, ustore_database_compact_t, ustore_snapshot_init_t,
    ustore_snapshot_save_t, ustore_collection_list_t, ustore_collection_create_t, ustore_collection_find_t,
    ustore_collection_drop_t, ustore_transaction_init_t, ustore_transaction_stage_t, ustore_transaction_commit_t,
    ustore_transaction_rollback_t, ustore_blobs_write_t, ustore_blobs_remove_t, ustore_blobs_read_t,
    ustore_blobs_scan_t, ustore_blobs_sample_t, ustore_blobs_measure_t, ustore_docs_write_t, ustore_docs_read_t,
    ustore_docs_gist_t, ustore_docs_gather_t, ustore_graph_read_edges_t, ustore_graph_write_edges_t,
    ustore_graph_remove_edges_t, ustore_graph_write_vertices_t, ustore_graph_remove_vertices_t, ustore_pages_import_t,
    ustore_pages_insert_t, ustore_pages_remove_t, ustore_pages_move_t, ustore_pages_splice_t,
    ustore_pages_set_attributes_t, ustore_pages_remove_attributes_t, ustore_pages_read_t, ustore_pages_history_t,
    ustore_strings_write_t, ustore_strings_remove_t, ustore_strings_read_t, ustore_strings_scan_t,
    ustore_blobs_cursor_next_t, ustore_strings_cursor_next_t, routed_init<ustore_database_init_ram_t>,
    routed_init<ustore_database_open_ram_t>, routed_init<ustore_database_open_rocksdb_t>,
    routed_init<ustore_database_view_rocksdb_t>, routed_init<ustore_database_open_leveldb_t>>;

/** What a deferred call leaves its driver, which publishes it once the call's table position is free again. */
struct deferred_result_t {

    /** The caller's outcome, which the driver finishes. */
    ustore_outcome_t* outcome = nullptr;

    /** What the engine answered. */
    status_t status;

    /** The deferred counts the call raised at submission, which the driver lowers once the outcome is final. */
    std::array<std::uint32_t*, 3> deferred_counts {};
};

/** One slot of the deferred table: a copy of the caller's argument struct and the routing that runs it. */
struct deferred_slot_t {

    /** A copy of the caller's argument struct, whose pointees stay borrowed until the call is final. */
    deferred_arguments_t arguments;

    /** Serves @c arguments in a scratch into a result for the driver to publish, as `run_deferred` does. */
    void (*run)(void* arguments, deferred_result_t& result, ustore_arena_t scratch) noexcept = nullptr;

    /** The call's place among every call on the executor, blocking ones included. */
    std::uint32_t ticket = 0;

    /** The slot's table position plus one once filled, stored with release ordering. */
    std::uint32_t ready = 0;
};

/** Whether the driver keeps serving the deferred table. */
enum class driver_state_t : std::uint32_t {

    /** Serving every deferred call submitted. */
    serving_k,

    /** Returning once the table is empty, as @c ustore_executor_free asks. */
    stopping_k,

    /** Never started, in a build without OS threads, where every call runs at submission. */
    absent_k,
};

/** Where an executor runs calls: one pool thread, or the calling thread of a blocking call. */
struct runner_t {

    /** The thread running a call here, empty while idle, through `std::atomic_ref`. */
    std::thread::id thread;

    /** The scratch every call run here works in, freed with the executor. */
    ustore_arena_handle_t scratch;
};

/** The threads that run calls: a pool, owned or lent, the driver handing it batches, and the deferred table. */
struct call_executor_t {

    /** The pool that runs deferred calls. */
    std::variant<executor_t, lent_pool_t> pool;

    /** One thread, waiting on @c doorbell while the table is empty. */
    executor_t driver;

    /** A ring of power-of-two slots, of which @c deferred_limit hold calls at once. */
    st::basic_vector<deferred_slot_t> deferred;

    /** One result per call of the batch the driver serves, read after the batch's table positions are free. */
    st::basic_vector<deferred_result_t> results;

    /** One runner per pool thread, plus one for a blocking call and a batch the driver runs itself. */
    st::basic_vector<runner_t> runners;

    /** Tickets taken in the low half and table positions taken in the high half. */
    std::uint64_t submitted = 0;

    /** The driver's loop, which @c ustore_executor_free joins. */
    fu_generation_t generation = 0;

    /** Deferred calls the table holds at once. */
    std::uint32_t deferred_limit = 0;

    /** Table positions the driver finished with, freeing the slots behind it. */
    std::uint32_t head = 0;

    /** Every ticket below it is final. */
    std::uint32_t finished = 0;

    /** Rung after every submission, waking the driver. */
    std::uint32_t doorbell = 0;

    /** What the pool and the driver do once the table empties. */
    ustore_mood_t mood = ustore_mood_parked_k;

    /** Whether the driver keeps serving. */
    driver_state_t state = driver_state_t::serving_k;

    /** Where the executor computes, which every arena it makes records. */
    residency_t residency = residency_t::host_k;

    /** The memory domain every arena it makes lives on, the first one it serves. */
    fu_memory_domain_id_t memory_domain = 0;
};

#pragma endregion Executors

#pragma region Claims

/** Where calls on @p executor compute, where a null one runs them on the calling thread over host pages. */
inline residency_t residency_of(call_executor_t const* executor) noexcept {
    return executor ? executor->residency : residency_t::host_k;
}

/** Takes @p word for one call, answering whether no other call held it. */
inline bool take(claim_t& word) noexcept {
    return std::atomic_ref<claim_t> {word}.exchange(claim_t::held_k, std::memory_order_acquire) == claim_t::free_k;
}

/** Hands @p word back to the next call. */
inline void release(claim_t& word) noexcept {
    std::atomic_ref<claim_t> {word}.store(claim_t::free_k, std::memory_order_release);
}

/** The arena @p arguments fill, or null for a call without outputs in one. */
template <typename arguments_type_>
ustore_arena_t arena_of(arguments_type_ const& arguments) noexcept {
    if constexpr (requires { arguments.arena; }) return arguments.arena;
    else return nullptr;
}

/** The transaction @p arguments carry through their view or their transaction, or null. */
template <typename arguments_type_>
ustore_transaction_t transaction_of(arguments_type_ const& arguments) noexcept {
    view_t view;
    return decode_call(arguments, view) ? view.transaction : nullptr;
}

/** The deferred count of the cursor @p arguments pull, or of the transaction or snapshot they read through, or null
 *  for none, as the cursor's holds keep what it reads through alive. */
template <typename arguments_type_>
std::uint32_t* named_calls_of(arguments_type_ const& arguments) noexcept {
    if constexpr (requires { arguments.cursor; }) return &arguments.cursor->deferred_calls;
    else {
        view_t view;
        decode_call(arguments, view);
        void* const read_through = view.transaction ? static_cast<void*>(view.transaction) : view.snapshot;
        return read_through ? &handle_cast<handle_t>(read_through)->deferred_calls : nullptr;
    }
}

/** The deferred counts of the database, the transaction, snapshot or cursor, and the arena @p arguments name, each
 *  null when absent. */
template <typename arguments_type_>
std::array<std::uint32_t*, 3> deferred_counts_of(arguments_type_ const& arguments) noexcept {
    ustore_arena_t const arena = arena_of(arguments);
    ustore_database_t const database = database_of(arguments);
    return {database ? &handle_cast<handle_t>(database)->deferred_calls : nullptr, named_calls_of(arguments),
            arena ? &arena->deferred_calls : nullptr};
}

/** An init names no handle yet, only an output for the database it opens, so it counts nowhere. */
template <typename arguments_type_>
std::array<std::uint32_t*, 3> deferred_counts_of(routed_init<arguments_type_> const&) noexcept {
    return {};
}

/** The claim of the cursor @p arguments pull, or null for a call naming none. */
template <typename arguments_type_>
claim_t* cursor_claim_of(arguments_type_ const& arguments) noexcept {
    if constexpr (requires { arguments.cursor; }) return &arguments.cursor->claim;
    else return nullptr;
}

/** Claims the arena, the cursor and the transaction of @p arguments, holding none when one serves another call, and
 *  refusing a call that needs an arena or a cursor and names none, or whose database or arena lives where its executor
 *  does not compute. */
template <typename arguments_type_>
status_t take_claims(arguments_type_ const& arguments) noexcept {
    if constexpr (requires { arguments.cursor; })
        if (!arguments.cursor) return {status_code_t::invalid_argument_k, "The call needs a cursor"};
    residency_t const residency = residency_of(arguments.executor);
    ustore_database_t const database = database_of(arguments);
    if (database && handle_cast<handle_t>(database)->residency != residency)
        return {status_code_t::device_k, "The database lives where another executor computes"};
    ustore_arena_t const arena = arena_of(arguments);
    if constexpr (requires { arguments.arena; }) {
        if (!arena) return {status_code_t::invalid_argument_k, "The call needs an arena for its outputs"};
        if (arena->residency != residency)
            return {status_code_t::device_k, "The arena lives where another executor computes"};
        if (!take(arena->claim)) return {status_code_t::contended_k, "The arena serves another call"};
    }
    claim_t* const cursor = cursor_claim_of(arguments);
    if (cursor && !take(*cursor)) {
        if (arena) release(arena->claim);
        return {status_code_t::contended_k, "The cursor serves another pull"};
    }
    ustore_transaction_t const transaction = transaction_of(arguments);
    if (transaction && !take(handle_cast<handle_t>(transaction)->claim)) {
        if (arena) release(arena->claim);
        if (cursor) release(*cursor);
        return {status_code_t::contended_k, "The transaction serves another call"};
    }
    return {};
}

/** Releases the claims of @p arguments once the engine returned. */
template <typename arguments_type_>
void release_claims(arguments_type_ const& arguments) noexcept {
    if (ustore_arena_t const arena = arena_of(arguments)) release(arena->claim);
    if (claim_t* const cursor = cursor_claim_of(arguments)) release(*cursor);
    if (ustore_transaction_t const transaction = transaction_of(arguments))
        release(handle_cast<handle_t>(transaction)->claim);
}

/** Finishes @p arguments with @p status without serving them, releasing the claims the router took. */
template <typename arguments_type_>
void refuse(arguments_type_& arguments, status_t status) noexcept {
    release_claims(arguments);
    finish(*arguments.outcome, status);
}

/** Waits until no deferred call counted in @p calls is left, so the handle counting them may be freed. */
inline void wait_deferred_calls(std::uint32_t& calls) noexcept {
    std::atomic_ref<std::uint32_t> const counted {calls};
    for (std::uint32_t seen = counted.load(std::memory_order_acquire); seen != 0;
         seen = counted.load(std::memory_order_acquire))
        counted.wait(seen, std::memory_order_acquire);
}

#pragma endregion Claims

#pragma region Routing

/** The tickets taken, in the low half of @c call_executor_t::submitted. */
constexpr std::uint32_t tickets_of(std::uint64_t submitted) noexcept { return static_cast<std::uint32_t>(submitted); }

/** The table positions taken, in the high half of @c call_executor_t::submitted. */
constexpr std::uint32_t positions_of(std::uint64_t submitted) noexcept {
    return static_cast<std::uint32_t>(submitted >> 32);
}

/** Packs @p tickets and @p positions into one word, so one compare-and-swap takes both. */
constexpr std::uint64_t submitted_of(std::uint32_t tickets, std::uint32_t positions) noexcept {
    return (static_cast<std::uint64_t>(positions) << 32) | tickets;
}

/** Whether @p counter has reached @p ticket, comparing 32-bit counters that wrap. */
constexpr bool reached(std::uint32_t counter, std::uint32_t ticket) noexcept {
    return static_cast<std::int32_t>(counter - ticket) >= 0;
}

/** Waits until every ticket below @p ticket is final. */
inline void wait_finished(call_executor_t& executor, std::uint32_t ticket) noexcept {
    std::atomic_ref<std::uint32_t> const finished {executor.finished};
    for (std::uint32_t seen = finished.load(std::memory_order_acquire); !reached(seen, ticket);
         seen = finished.load(std::memory_order_acquire))
        finished.wait(seen, std::memory_order_acquire);
}

/** Marks every ticket below @p ticket final and wakes whoever waits for one. */
inline void advance_finished(call_executor_t& executor, std::uint32_t ticket) noexcept {
    std::atomic_ref<std::uint32_t> const finished {executor.finished};
    finished.store(ticket, std::memory_order_release);
    finished.notify_all();
}

/** Whether the calling thread runs a call of @p executor, so waiting on the executor would wait on itself. */
inline bool runs_here(call_executor_t& executor) noexcept {
    std::thread::id const self = std::this_thread::get_id();
    for (runner_t& runner : executor.runners)
        if (std::atomic_ref<std::thread::id> {runner.thread}.load(std::memory_order_relaxed) == self) return true;
    return false;
}

/** Runs @p body on the calling thread over the scratch of @p runner, recorded there while it does. */
template <typename body_type_>
void run_as(runner_t& runner, body_type_&& body) noexcept {
    std::atomic_ref<std::thread::id> const recorded {runner.thread};
    recorded.store(std::this_thread::get_id(), std::memory_order_relaxed);
    body(&runner.scratch);
    recorded.store(std::thread::id {}, std::memory_order_relaxed);
}

/** The right to dispatch on @p pool, owned or lent. */
inline pool_view_t view_of(std::variant<executor_t, lent_pool_t> const& pool) noexcept {
    return std::visit([](auto const& alternative) noexcept { return alternative.view(); }, pool);
}

/** Serves @p arguments on the engine of @p slot_ in @p scratch and releases their claims, leaving the outputs of a
 *  failed call null or zero, and answers the status their outcome is to finish with. */
template <typename arguments_type_, auto slot_>
status_t serve_and_release(arguments_type_& arguments, ustore_arena_t scratch) noexcept {
    status_t const status = serve<slot_>(arguments, scratch);
    if constexpr (requires { clear_outputs(arguments); })
        if (status.failed()) clear_outputs(arguments);
    release_claims(arguments);
    return status;
}

/** Opens the database @p call names through its engine's open, which claims nothing. */
template <typename arguments_type_>
status_t serve_open(routed_init<arguments_type_>& call, ustore_arena_t scratch) noexcept {
    return call.open(call, view_t {.scratch = scratch});
}

/** Serves @p call with @p serve_ in @p scratch, then finishes its outcome. */
template <typename call_type_, status_t (*serve_)(call_type_&, ustore_arena_t) noexcept>
void run(call_type_* call, ustore_arena_t scratch) noexcept {
    finish(*call->outcome, serve_(*call, scratch));
}

/** Runs @p call on the calling thread over a scratch made for it alone, as a call without an executor keeping scratch
 *  needs: its first page lies on the stack, so a small call allocates nothing, and the blocks it grows into are freed
 *  once the call is final. */
template <typename call_type_, status_t (*serve_)(call_type_&, ustore_arena_t) noexcept>
void run_alone(call_type_* call) noexcept {
    alignas(arena_alignment_k) byte_t page[arena_page_bytes_k];
    arena_block_t* const first = new (page) arena_block_t {nullptr, sizeof(page), sizeof(arena_block_t)};
    ustore_arena_handle_t scratch {.newest = first};
    run<call_type_, serve_>(call, &scratch);
    arena_free_blocks(scratch, first);
}

/** Serves the copy @p erased holds in @p scratch into @p result, leaving its outcome and its deferred counts to the
 *  driver, which frees the copy's table position first. */
template <typename call_type_, status_t (*serve_)(call_type_&, ustore_arena_t) noexcept>
void run_deferred(void* erased, deferred_result_t& result, ustore_arena_t scratch) noexcept {
    call_type_& call = *std::get_if<call_type_>(static_cast<deferred_arguments_t*>(erased));
    // Counted before the call runs, as a transaction init rewrites the handle its arguments point to
    result.deferred_counts = deferred_counts_of(call);
    result.outcome = call.outcome;
    result.status = serve_(call, scratch);
}

/** Runs @p call on the calling thread after every call submitted before it finishes, and before any after. */
template <typename call_type_, status_t (*serve_)(call_type_&, ustore_arena_t) noexcept>
void run_blocking(call_executor_t& executor, call_type_* call) noexcept {
    if (runs_here(executor))
        return refuse(*call, {status_code_t::contended_k, "A callback cannot block on the executor running it"});
    std::atomic_ref<std::uint64_t> const submitted {executor.submitted};
    std::uint64_t observed = submitted.load(std::memory_order_relaxed);
    while (!submitted.compare_exchange_weak(observed, submitted_of(tickets_of(observed) + 1, positions_of(observed)),
                                            std::memory_order_relaxed)) {}
    std::uint32_t const ticket = tickets_of(observed);
    wait_finished(executor, ticket);
    run_as(executor.runners.back(),
           [call](ustore_arena_t scratch) noexcept { run<call_type_, serve_>(call, scratch); });
    advance_finished(executor, ticket + 1);
}

/** Copies @p call into the deferred table of @p executor, finishing it contended when the table is full. */
template <typename call_type_, status_t (*serve_)(call_type_&, ustore_arena_t) noexcept>
void submit(call_executor_t& executor, call_type_* call) noexcept {
    if (std::atomic_ref<driver_state_t> {executor.state}.load(std::memory_order_relaxed) == driver_state_t::absent_k)
        return run_alone<call_type_, serve_>(call);

    std::atomic_ref<ustore_status_t> {call->outcome->status}.store(ustore_status_deferred_k, std::memory_order_relaxed);
    std::atomic_ref<std::uint64_t> const submitted {executor.submitted};
    std::atomic_ref<std::uint32_t> const head {executor.head};
    std::uint64_t observed = submitted.load(std::memory_order_relaxed);
    do {
        // A stale `observed` reads as a negative count and fails the exchange below
        std::uint32_t const held = positions_of(observed) - head.load(std::memory_order_acquire);
        if (static_cast<std::int32_t>(held) >= static_cast<std::int32_t>(executor.deferred_limit))
            return refuse(*call, {status_code_t::contended_k, "The executor's deferred table is full"});
    } while (!submitted.compare_exchange_weak(
        observed, submitted_of(tickets_of(observed) + 1, positions_of(observed) + 1), std::memory_order_relaxed));

    std::uint32_t const position = positions_of(observed);
    for (std::uint32_t* const count : deferred_counts_of(*call))
        if (count) std::atomic_ref<std::uint32_t> {*count}.fetch_add(1, std::memory_order_relaxed);
    deferred_slot_t& slot = executor.deferred[position & (executor.deferred.size() - 1)];
    slot.arguments.template emplace<call_type_>(*call);
    slot.run = &run_deferred<call_type_, serve_>;
    slot.ticket = tickets_of(observed);
    std::atomic_ref<std::uint32_t> {slot.ready}.store(position + 1, std::memory_order_release);
    std::atomic_ref<std::uint32_t> const doorbell {executor.doorbell};
    doorbell.fetch_add(1, std::memory_order_release);
    doorbell.notify_one();
}

/** Runs @p call on the calling thread, blocking on its executor, or submits it, as its executor and completion ask. */
template <typename call_type_, status_t (*serve_)(call_type_&, ustore_arena_t) noexcept>
void dispatch(call_type_* call) noexcept {
    call_executor_t* const executor = call->executor;
    if (!executor) return run_alone<call_type_, serve_>(call);
    if (call->completion == ustore_completion_blocking_k) return run_blocking<call_type_, serve_>(*executor, call);
    submit<call_type_, serve_>(*executor, call);
}

/** Leaves the outputs of @p arguments null or zero and claims what they hold, then dispatches them to @p slot_. */
template <auto slot_, typename arguments_type_>
void route(arguments_type_* arguments) noexcept {
    if constexpr (requires { clear_outputs(*arguments); }) clear_outputs(*arguments);
    if (status_t const claimed = take_claims(*arguments); claimed.failed()) return finish(*arguments->outcome, claimed);
    dispatch<arguments_type_, &serve_and_release<arguments_type_, slot_>>(arguments);
}

template <typename arguments_type_>
void route_init(arguments_type_* arguments, status_t (*open)(arguments_type_ const&, view_t const&) noexcept) noexcept {
    if (arguments->database) *arguments->database = nullptr;
    routed_init<arguments_type_> call {*arguments, open};
    dispatch<routed_init<arguments_type_>, &serve_open<arguments_type_>>(&call);
}

#pragma endregion Routing

#pragma region Cursor Lifetimes

/** Opens a cursor of @p cursor_type_ over the walk @p arguments describe, holding their database and the transaction
 *  or snapshot their view reads through, and refusing a database whose engine lacks @p slot_ or a view of another
 *  database. */
template <typename cursor_type_, auto slot_, typename arguments_type_>
expected<cursor_type_*> open_cursor(arguments_type_ const& arguments) noexcept {
    if (!arguments.cursor)
        return status_t {status_code_t::invalid_argument_k, "The call needs an output for the cursor"};
    *arguments.cursor = nullptr;
    ustore_database_t const database = arguments.database;
    if (!database) return status_t {status_code_t::uninitialized_k, "The database is uninitialized"};
    if (!(engine_of(database).*slot_))
        return status_t {status_code_t::unsupported_k, "The engine does not implement this call"};
    view_t view;
    if (!decode_view(arguments.view, view))
        return status_t {status_code_t::invalid_argument_k, "The view was made by neither view constructor"};
    void* const parent = view.transaction ? static_cast<void*>(view.transaction) : view.snapshot;
    if (parent && handle_cast<handle_t>(parent)->database != database)
        return status_t {status_code_t::invalid_argument_k, "The view reads another database"};

    cursor_type_* const cursor = new (std::nothrow) cursor_type_ {};
    if (!cursor) return status_t {status_code_t::out_of_memory_k, "Failed to allocate the cursor"};
    cursor->database = database;
    cursor->view = arguments.view;
    cursor->collection = arguments.collection;
    take_hold(database);
    if (parent) take_hold(parent);
    return cursor;
}

/** Waits until no deferred pull of @p cursor is left, then gives back its holds and frees it, where null is safe. */
template <typename cursor_type_>
void free_cursor(cursor_type_* cursor) noexcept {
    if (!cursor) return;
    wait_deferred_calls(cursor->deferred_calls);
    view_t view;
    decode_view(cursor->view, view);
    release_parent(view);
    release_hold(cursor->database);
    delete cursor;
}

#pragma endregion Cursor Lifetimes

} // namespace unum::ustore

using namespace unum::ustore;

/** What @c ustore_blobs_cursor_t points to. */
struct ustore_blobs_cursor_handle_t : blobs_cursor_t {};

/** What @c ustore_strings_cursor_t points to. */
struct ustore_strings_cursor_handle_t : strings_cursor_t {};

/** What @c ustore_executor_t points to. */
struct ustore_executor_handle_t : call_executor_t {};
