/**
 *  @file src/lib.cpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief The C ABI, routing every call to the engine behind its database or transaction handle.
 *
 *  Each engine defines only its own `ustore_database_init_*`, `ustore_database_open_*` and `ustore_database_view_*`,
 *  which stamp every handle they hand out with its @c engine_t, so one library holds every engine it was built with
 *  and a program links only those it opens. The opens of an engine the build lacks are defined here and refuse, so
 *  every program links against every build, and every other one hands its engine's open to @c route_init, which routes
 *  it as any call. Every call of the table is `noexcept`, so nothing here catches. The router claims the arena and the
 *  transaction of a call, refuses a change to a database opened read-only, then runs the call on the calling thread,
 *  blocking on its executor, or copies it into the executor's deferred table, and finishes its outcome only after the
 *  engine returns.
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

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`, `std::uintptr_t`
#include <cstring> // `std::memcpy`

#if defined(_WIN32)
#include <windows.h> // `NTSTATUS`, which <bcrypt.h> needs
#include <bcrypt.h>  // `BCryptGenRandom`
#else
#include <sys/random.h> // `getentropy`
#endif

#include <algorithm>   // `std::min`, `std::reverse`
#include <array>       // `std::array`
#include <atomic>      // `std::atomic_ref`, `std::memory_order_release`
#include <bit>         // `std::bit_ceil`, `std::bit_cast`
#include <chrono>      // `std::chrono::system_clock`
#include <new>         // `std::nothrow`, placement `new`
#include <numeric>     // `std::iota`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <thread>      // `std::this_thread::get_id`, `std::thread::id`
#include <tuple>       // `std::apply`
#include <type_traits> // `std::underlying_type_t`
#include <variant>     // `std::variant`, `std::visit`, `std::get_if`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`
#include <stringzilla/stringzilla.hpp> // `sz::string_t`

#include "ustore/types.hpp" // `key_after`, `largest_key_k`

#include "engine.hpp"                // `engine_t`, `handle_t`, `view_t`
#include "utilities/batches.hpp"     // `strings_arg_t`
#include "utilities/progress.hpp"    // `progress_t`, `progress_cancelled_k`
#include "utilities/ram_buffers.hpp" // `arena_rewind`, `arena_copy`, `arena_page_bytes_k`, `export_strings`
#include "utilities/ram_device.hpp"  // `executor_t`, `lent_pool_t`, `topology_t`, `claim_t`, `residency_t`

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

/** The canonical spelling of every status, indexed by its value. */
constexpr std::array<std::string_view, ustore_status_read_only_k + 1> status_names_k {
    "success",
    "deferred",
    "out-of-memory",
    "invalid-argument",
    "uninitialized",
    "unsupported",
    "io",
    "malformed",
    "not-found",
    "already-exists",
    "write-conflict",
    "read-conflict",
    "phantom-conflict",
    "capacity-exhausted",
    "device",
    "internal",
    "contended",
    "cancelled",
    "read-only",
};

/** One enumerator of a closed set and its canonical spelling. */
template <typename enum_type_>
struct enumerator_spelling {
    enum_type_ value;
    std::string_view name;
};

/** The canonical spelling of every mood. */
constexpr enumerator_spelling<ustore_mood_t> mood_names_k[] {
    {ustore_mood_parked_k, "parked"},
    {ustore_mood_spinning_k, "spinning"},
};

/** The canonical spelling of every durability. */
constexpr enumerator_spelling<ustore_durability_t> durability_names_k[] {
    {ustore_durability_buffered_k, "buffered"},
    {ustore_durability_flushed_k, "flushed"},
};

/** The canonical spelling of every watch. */
constexpr enumerator_spelling<ustore_watch_t> watch_names_k[] {
    {ustore_watch_keys_k, "keys"},
    {ustore_watch_none_k, "none"},
};

/** The canonical spelling of every scan mode. */
constexpr enumerator_spelling<ustore_scan_mode_t> scan_mode_names_k[] {
    {ustore_scan_ordered_k, "ordered"},
    {ustore_scan_unordered_k, "unordered"},
};

/** The canonical spelling of every drop mode. */
constexpr enumerator_spelling<ustore_drop_mode_t> drop_mode_names_k[] {
    {ustore_drop_collection_k, "collection"},
    {ustore_drop_pairs_k, "pairs"},
    {ustore_drop_values_k, "values"},
};

/** The canonical spelling of every isolation. */
constexpr enumerator_spelling<ustore_isolation_t> isolation_names_k[] {
    {ustore_isolation_strict_serializable_k, "strict-serializable"},
    {ustore_isolation_snapshot_k, "snapshot"},
};

/** The canonical spelling of every transport. */
constexpr enumerator_spelling<ustore_transport_t> transport_names_k[] {
    {ustore_transport_automatic_k, "automatic"},     {ustore_transport_blocking_k, "blocking"},
    {ustore_transport_linux_uring_k, "linux-uring"}, {ustore_transport_linux_aio_k, "linux-aio"},
    {ustore_transport_linux_spdk_k, "linux-spdk"},
};

/** The canonical spelling of every engine. */
constexpr enumerator_spelling<ustore_engine_t> engine_names_k[] {
    {ustore_engine_ram_k, "ram"},
    {ustore_engine_rocksdb_k, "rocksdb"},
    {ustore_engine_leveldb_k, "leveldb"},
    {ustore_engine_nvme_k, "nvme"},
};

/** The canonical spelling of every modality. */
constexpr enumerator_spelling<ustore_modality_t> modality_names_k[] {
    {ustore_modality_any_k, "any"},     {ustore_modality_blobs_k, "blobs"},     {ustore_modality_docs_k, "docs"},
    {ustore_modality_graph_k, "graph"}, {ustore_modality_strings_k, "strings"}, {ustore_modality_pages_k, "pages"},
};

/** The canonical spelling of every vertex role. */
constexpr enumerator_spelling<ustore_vertex_role_t> vertex_role_names_k[] {
    {ustore_vertex_role_unknown_k, "unknown"},
    {ustore_vertex_role_source_k, "source"},
    {ustore_vertex_role_target_k, "target"},
    {ustore_vertex_role_any_k, "any"},
};

/** The canonical spelling of every document field type. */
constexpr enumerator_spelling<ustore_docs_field_type_t> docs_field_type_names_k[] {
    {ustore_docs_field_type_json_k, "json"}, {ustore_docs_field_type_null_k, "null"},
    {ustore_docs_field_type_bool_k, "bool"}, {ustore_docs_field_type_uuid_k, "uuid"},
    {ustore_docs_field_type_f64_k, "f64"},   {ustore_docs_field_type_f32_k, "f32"},
    {ustore_docs_field_type_f16_k, "f16"},   {ustore_docs_field_type_i64_k, "i64"},
    {ustore_docs_field_type_i32_k, "i32"},   {ustore_docs_field_type_i16_k, "i16"},
    {ustore_docs_field_type_i8_k, "i8"},     {ustore_docs_field_type_u64_k, "u64"},
    {ustore_docs_field_type_u32_k, "u32"},   {ustore_docs_field_type_u16_k, "u16"},
    {ustore_docs_field_type_u8_k, "u8"},     {ustore_docs_field_type_bin_k, "bin"},
    {ustore_docs_field_type_str_k, "str"},
};

/** The canonical spelling of every document modification. */
constexpr enumerator_spelling<ustore_docs_modification_t> docs_modification_names_k[] {
    {ustore_docs_modify_upsert_k, "upsert"}, {ustore_docs_modify_update_k, "update"},
    {ustore_docs_modify_insert_k, "insert"}, {ustore_docs_modify_patch_k, "patch"},
    {ustore_docs_modify_merge_k, "merge"},
};

/** The canonical spelling of every page encoding. */
constexpr enumerator_spelling<ustore_page_encoding_t> page_encoding_names_k[] {
    {ustore_page_encoding_html_k, "html"},         {ustore_page_encoding_xml_k, "xml"},
    {ustore_page_encoding_markdown_k, "markdown"}, {ustore_page_encoding_text_k, "text"},
    {ustore_page_encoding_json_k, "json"},
};

/** The canonical spelling of every page modification. */
constexpr enumerator_spelling<ustore_page_modification_t> page_modification_names_k[] {
    {ustore_page_modify_import_k, "import"},
    {ustore_page_modify_insert_k, "insert"},
    {ustore_page_modify_remove_k, "remove"},
    {ustore_page_modify_move_k, "move"},
    {ustore_page_modify_splice_k, "splice"},
    {ustore_page_modify_set_attributes_k, "set-attributes"},
    {ustore_page_modify_remove_attributes_k, "remove-attributes"},
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

/** The version as "major.minor.patch", spelled from the version macros at compile time. */
constexpr auto version_name_k = [] {
    std::array<char, 3 * 11 + 3> text {};
    std::size_t length = 0;
    for (int part : {USTORE_VERSION_MAJOR, USTORE_VERSION_MINOR, USTORE_VERSION_PATCH}) {
        if (length) text[length++] = '.';
        std::size_t const first = length;
        do text[length++] = static_cast<char>('0' + part % 10);
        while (part /= 10);
        std::reverse(text.begin() + first, text.begin() + length);
    }
    return text;
}();

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
ustore_view_t tagged_view(void* handle, view_tag_t tag) noexcept {
    if (!handle) return nullptr;
    return reinterpret_cast<ustore_view_t>(reinterpret_cast<std::uintptr_t>(handle) | static_cast<std::uintptr_t>(tag));
}

/** Decodes @p view into @p decoded, answering false for a pointer neither view constructor made. */
bool decode_view(ustore_view_t view, view_t& decoded) noexcept {
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
ustore_transaction_t carried(ustore_transaction_t transaction) noexcept { return transaction; }

/** The transaction a call reuses, or null for one it opens afresh. */
ustore_transaction_t carried(ustore_transaction_t const* transaction) noexcept {
    return transaction ? *transaction : nullptr;
}

/** The snapshot a call reads, or null, which an output for a new snapshot never matches. */
ustore_snapshot_t viewed(ustore_snapshot_t snapshot) noexcept { return snapshot; }

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
void take_hold(void* handle) noexcept {
    std::atomic_ref<std::uint32_t> {handle_cast<handle_t>(handle)->holds}.fetch_add(1, std::memory_order_relaxed);
}

/** Drops one hold on @p handle, answering whether it was the last. */
bool drop_hold(void* handle) noexcept {
    std::atomic_ref<std::uint32_t> const holds {handle_cast<handle_t>(handle)->holds};
    return holds.fetch_sub(1, std::memory_order_acq_rel) == 1;
}

/** Keeps @p database open until @p opened, a transaction or a snapshot of it, is freed. */
void hold(ustore_database_t database, void* opened) noexcept {
    handle_cast<handle_t>(opened)->database = database;
    take_hold(database);
}

/** Drops one hold on @p database, if any, closing it through its engine with the last. */
void release_hold(ustore_database_t database) noexcept {
    if (database && drop_hold(database)) engine_of(database).database_free(database);
}

/** Drops one hold on the transaction or snapshot @p view names, if any, freeing it through its engine with the last,
 *  which gives back the hold it kept on its database. */
void release_parent(view_t const& view) noexcept {
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
void clear_outputs(ustore_database_describe_t const& arguments) noexcept { clear(arguments.description); }

void clear_outputs(ustore_snapshot_init_t const& arguments) noexcept {
    clear(arguments.snapshot), clear(arguments.read_stamp);
}

void clear_outputs(ustore_collection_list_t const& arguments) noexcept {
    clear(arguments.collections), clear(arguments.modalities), clear(arguments.names), clear(arguments.written);
}

void clear_outputs(ustore_collection_create_t const& arguments) noexcept { clear(arguments.collection); }

void clear_outputs(ustore_collection_find_t const& arguments) noexcept {
    clear(arguments.presences), clear(arguments.collections), clear(arguments.modalities);
}

void clear_outputs(ustore_transaction_stage_t const& arguments) noexcept { clear(arguments.read_stamp); }

void clear_outputs(ustore_transaction_commit_t const& arguments) noexcept { clear(arguments.commit_stamp); }

void clear_outputs(ustore_blobs_read_t const& arguments) noexcept {
    clear(arguments.presences), clear(arguments.lengths), clear(arguments.values);
}

void clear_outputs(ustore_blobs_scan_t const& arguments) noexcept { clear(arguments.offsets), clear(arguments.keys); }

void clear_outputs(ustore_blobs_sample_t const& arguments) noexcept { clear(arguments.offsets), clear(arguments.keys); }

void clear_outputs(ustore_blobs_measure_t const& arguments) noexcept {
    clear(arguments.min_cardinalities), clear(arguments.max_cardinalities), clear(arguments.min_value_bytes);
    clear(arguments.max_value_bytes), clear(arguments.min_space_usages), clear(arguments.max_space_usages);
}

void clear_outputs(ustore_docs_read_t const& arguments) noexcept {
    clear(arguments.presences), clear(arguments.lengths), clear(arguments.values);
}

void clear_outputs(ustore_docs_gist_t const& arguments) noexcept { clear(arguments.fields), clear(arguments.written); }

void clear_outputs(ustore_docs_gather_t const& arguments) noexcept { clear(arguments.columns); }

void clear_outputs(ustore_graph_read_edges_t const& arguments) noexcept {
    clear(arguments.degrees_per_vertex), clear(arguments.edges_offsets), clear(arguments.edges_per_vertex);
}

void clear_outputs(ustore_pages_insert_t const& arguments) noexcept { clear(arguments.effective_keys); }

void clear_outputs(ustore_pages_read_t const& arguments) noexcept {
    clear(arguments.presences), clear(arguments.lengths), clear(arguments.values);
}

void clear_outputs(ustore_pages_history_t const& arguments) noexcept {
    clear(arguments.edits_offsets), clear(arguments.stamps), clear(arguments.modifications), clear(arguments.elements);
    clear(arguments.parents), clear(arguments.anchors), clear(arguments.fields), clear(arguments.text_starts);
    clear(arguments.text_lengths), clear(arguments.values);
}

void clear_outputs(ustore_strings_read_t const& arguments) noexcept {
    clear(arguments.presences), clear(arguments.lengths), clear(arguments.values);
}

void clear_outputs(ustore_strings_scan_t const& arguments) noexcept {
    clear(arguments.offsets), clear(arguments.strings);
}

void clear_outputs(ustore_blobs_cursor_next_t const& arguments) noexcept {
    clear(arguments.keys), clear(arguments.values), clear(arguments.written);
}

void clear_outputs(ustore_strings_cursor_next_t const& arguments) noexcept {
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
std::array<ragged_input_t, 1> ragged_inputs(ustore_collection_find_t const& arguments) noexcept {
    return {{{arguments.names, arguments.count}}};
}

std::array<ragged_input_t, 1> ragged_inputs(ustore_blobs_write_t const& arguments) noexcept {
    return {{{arguments.values, arguments.count}}};
}

std::array<ragged_input_t, 2> ragged_inputs(ustore_docs_write_t const& arguments) noexcept {
    return {{{arguments.fields, arguments.count}, {arguments.values, arguments.count}}};
}

std::array<ragged_input_t, 1> ragged_inputs(ustore_docs_read_t const& arguments) noexcept {
    return {{{arguments.fields, arguments.count}}};
}

std::array<ragged_input_t, 1> ragged_inputs(ustore_docs_gather_t const& arguments) noexcept {
    return {{{arguments.fields, arguments.fields_count}}};
}

std::array<ragged_input_t, 1> ragged_inputs(ustore_pages_import_t const& arguments) noexcept {
    return {{{arguments.values, arguments.count}}};
}

std::array<ragged_input_t, 1> ragged_inputs(ustore_pages_insert_t const& arguments) noexcept {
    return {{{arguments.values, arguments.count}}};
}

std::array<ragged_input_t, 1> ragged_inputs(ustore_pages_splice_t const& arguments) noexcept {
    return {{{arguments.values, arguments.count}}};
}

std::array<ragged_input_t, 2> ragged_inputs(ustore_pages_set_attributes_t const& arguments) noexcept {
    return {{{arguments.fields, arguments.count}, {arguments.values, arguments.count}}};
}

std::array<ragged_input_t, 1> ragged_inputs(ustore_pages_remove_attributes_t const& arguments) noexcept {
    return {{{arguments.fields, arguments.count}}};
}

std::array<ragged_input_t, 2> ragged_inputs(ustore_strings_write_t const& arguments) noexcept {
    return {{{arguments.strings, arguments.count}, {arguments.values, arguments.count}}};
}

std::array<ragged_input_t, 1> ragged_inputs(ustore_strings_remove_t const& arguments) noexcept {
    return {{{arguments.strings, arguments.count}}};
}

std::array<ragged_input_t, 1> ragged_inputs(ustore_strings_read_t const& arguments) noexcept {
    return {{{arguments.strings, arguments.count}}};
}

std::array<ragged_input_t, 2> ragged_inputs(ustore_strings_scan_t const& arguments) noexcept {
    return {{{arguments.prefixes, arguments.count}, {arguments.start_strings, arguments.count}}};
}

/** Strings an accessor writes per block, so the indices it reads fit on the stack of the thread running the call. */
constexpr std::size_t accessor_block_k = 64;

/** Reads @p input for its engine into @p strings, refusing a layout no constant names and gathering into @p scratch
 *  the views an accessor writes, so the accessor runs on the thread running the call and never after it is final. */
status_t gather_strings(ragged_input_t const& input, ustore_arena_t scratch, strings_arg_t& strings) noexcept {
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
              view_t const& view) noexcept {
    if (!arguments.keys || !arguments.written)
        return {status_code_t::invalid_argument_k, "Need outputs for the keys and the count written"};
    if (arguments.values && !engine.blobs_read)
        return {status_code_t::unsupported_k, "The engine does not implement this call"};
    ustore_length_t const* offsets = nullptr;
    ustore_key_t const* listed = nullptr;
    if (!cursor.exhausted && arguments.wanted) {
        ustore_blobs_scan_t const scan {.database = arguments.database,
                                        .count = 1,
                                        .collections = {&cursor.collection, 0},
                                        .start_keys = {&cursor.start_key, 0},
                                        .offsets = &offsets,
                                        .keys = &listed,
                                        .arena = view.scratch,
                                        .wanted = arguments.wanted};
        if (status_t const status = engine.blobs_scan(scan, view); status.failed()) return status;
    }
    std::size_t const written = offsets ? offsets[1] : 0;
    if (written && arguments.values) {
        ustore_blobs_read_t const read {.database = arguments.database,
                                        .count = written,
                                        .collections = {&cursor.collection, 0},
                                        .keys = {listed, sizeof(ustore_key_t)},
                                        .values = arguments.values,
                                        .arena = arguments.arena};
        if (status_t const status = engine.blobs_read(read, view); status.failed()) return status;
    }
    auto [keys, keys_status] = arena_copy<ustore_key_t>(arguments.arena, {listed, written});
    if (keys_status.failed()) return keys_status;
    if (!progress_t {arguments.progress, arguments.progress_state, 1}.advance(1)) return progress_cancelled_k;

    // A page short of what it wanted reached the end of the collection, and so did one ending at the last key
    ustore_key_t const* const last = written ? listed + written - 1 : nullptr;
    if (written < arguments.wanted || (last && *last == largest_key_k)) cursor.exhausted = true;
    else if (last) cursor.start_key = key_after(*last);
    *arguments.keys = keys.data();
    *arguments.written = written;
    return {};
}

/** Pulls the next page of the walk @p arguments name through @p engine, as the blobs pull does, resuming past the last
 *  string at that string followed by a zero byte, the smallest string after it. */
status_t pull(engine_t const& engine, ustore_strings_cursor_next_t const& arguments, strings_cursor_t& cursor,
              view_t const& view) noexcept {
    if (!arguments.strings || !arguments.written)
        return {status_code_t::invalid_argument_k, "Need outputs for the strings and the count written"};
    if (arguments.values && !engine.strings_read)
        return {status_code_t::unsupported_k, "The engine does not implement this call"};
    ustore_bytes_view_t const prefix = bytes_of(std::string_view {cursor.prefix});
    ustore_bytes_view_t const start = bytes_of(std::string_view {cursor.start});
    ustore_length_t const* offsets = nullptr;
    strings_arg_t listed;
    if (!cursor.exhausted && arguments.wanted) {
        ustore_strings_scan_t const scan {.database = arguments.database,
                                          .count = 1,
                                          .collections = {&cursor.collection, 0},
                                          .offsets = &offsets,
                                          .strings = &listed.view,
                                          .arena = view.scratch,
                                          .wanted = arguments.wanted};
        status_t const status = engine.strings_scan(scan, strings_arg_t::from_views(&prefix),
                                                    strings_arg_t::from_views(&start), view);
        if (status.failed()) return status;
    }
    std::size_t const written = offsets ? offsets[1] : 0;
    if (written && arguments.values) {
        ustore_strings_read_t const read {.database = arguments.database,
                                          .count = written,
                                          .collections = {&cursor.collection, 0},
                                          .values = arguments.values,
                                          .arena = arguments.arena};
        if (status_t const status = engine.strings_read(read, listed, view); status.failed()) return status;
    }
    std::size_t bytes = 0;
    for (std::size_t index = 0; index != written; ++index) bytes += listed[index].length;
    auto const strings = [&](std::size_t index) noexcept { return listed[index]; };
    if (status_t const exported = export_strings(arguments.arena, written, bytes, strings, *arguments.strings);
        exported.failed())
        return exported;
    ustore_bytes_view_t const last = written ? listed[written - 1] : ustore_bytes_view_t {};
    if (written && cursor.start.try_reserve(last.length + 1) != sz::status_t::success_k)
        return {status_code_t::out_of_memory_k, "Failed to keep the string the walk resumes past"};
    if (!progress_t {arguments.progress, arguments.progress_state, 1}.advance(1)) return progress_cancelled_k;

    // Reserved above, so the resize allocates nothing once the callback let the pull finish
    if (written < arguments.wanted) cursor.exhausted = true;
    else if (written && cursor.start.try_resize(last.length + 1) == sz::status_t::success_k) {
        if (last.length) std::memcpy(cursor.start.data(), last.data, last.length);
        cursor.start[last.length] = 0;
    }
    *arguments.written = written;
    return {};
}

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
void finish(ustore_outcome_t& outcome, status_t status) noexcept {
    outcome.message = status.message;
    std::atomic_ref<ustore_status_t> const word {outcome.status};
    word.store(static_cast<ustore_status_t>(status.code), std::memory_order_release);
    word.notify_all();
}

expected<ustore_collection_id_t> mint_collection_id() noexcept {
    ustore_collection_id_t id {};
    std::uint8_t* const bytes = id.uuid.bytes;
#if defined(_WIN32)
    bool const drawn = BCRYPT_SUCCESS(
        BCryptGenRandom(nullptr, bytes, sizeof(id.uuid.bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
#else
    bool const drawn = getentropy(bytes, sizeof(id.uuid.bytes)) == 0;
#endif
    if (!drawn) return status_t {status_code_t::internal_k, "The system gave no entropy for a collection identifier"};
    // Version 7: Unix milliseconds big-endian in bytes [0, 6), then the version nibble and the RFC variant over the
    // random bits, so identifiers minted later order after earlier ones
    auto const now = std::chrono::system_clock::now().time_since_epoch();
    auto const milliseconds = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
    for (unsigned index = 0; index != 6; ++index)
        bytes[index] = static_cast<std::uint8_t>(milliseconds >> (40 - 8 * index));
    bytes[6] = static_cast<std::uint8_t>(0x70 | (bytes[6] & 0x0F));
    bytes[8] = static_cast<std::uint8_t>(0x80 | (bytes[8] & 0x3F));
    return id;
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
residency_t residency_of(call_executor_t const* executor) noexcept {
    return executor ? executor->residency : residency_t::host_k;
}

/** Takes @p word for one call, answering whether no other call held it. */
bool take(claim_t& word) noexcept {
    return std::atomic_ref<claim_t> {word}.exchange(claim_t::held_k, std::memory_order_acquire) == claim_t::free_k;
}

/** Hands @p word back to the next call. */
void release(claim_t& word) noexcept {
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
void wait_deferred_calls(std::uint32_t& calls) noexcept {
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
void wait_finished(call_executor_t& executor, std::uint32_t ticket) noexcept {
    std::atomic_ref<std::uint32_t> const finished {executor.finished};
    for (std::uint32_t seen = finished.load(std::memory_order_acquire); !reached(seen, ticket);
         seen = finished.load(std::memory_order_acquire))
        finished.wait(seen, std::memory_order_acquire);
}

/** Marks every ticket below @p ticket final and wakes whoever waits for one. */
void advance_finished(call_executor_t& executor, std::uint32_t ticket) noexcept {
    std::atomic_ref<std::uint32_t> const finished {executor.finished};
    finished.store(ticket, std::memory_order_release);
    finished.notify_all();
}

/** Whether the calling thread runs a call of @p executor, so waiting on the executor would wait on itself. */
bool runs_here(call_executor_t& executor) noexcept {
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
pool_view_t view_of(std::variant<executor_t, lent_pool_t> const& pool) noexcept {
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

/** Finishes the outcomes of the first @p count results of @p executor, each before lowering the deferred counts its
 *  call raised, so a free waiting on one of them returns after that outcome is final. */
void publish_results(call_executor_t& executor, std::uint32_t count) noexcept {
    for (std::uint32_t task = 0; task != count; ++task) {
        deferred_result_t const& result = executor.results[task];
        finish(*result.outcome, result.status);
        for (std::uint32_t* const deferred_calls : result.deferred_counts) {
            if (!deferred_calls) continue;
            std::atomic_ref<std::uint32_t> const calls {*deferred_calls};
            calls.fetch_sub(1, std::memory_order_release);
            calls.notify_all();
        }
    }
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

/** Runs the ready slots from @c head whose tickets follow one another as one batch, answering how many it ran.
 *
 *  The batch's table positions are freed before any of its outcomes is final, so a caller seeing one final may submit
 *  again at once. Nothing reads a slot after that, since a submission may refill it. */
std::uint32_t serve_batch(call_executor_t& executor) noexcept {
    std::uint32_t const head = std::atomic_ref<std::uint32_t> {executor.head}.load(std::memory_order_relaxed);
    std::size_t const mask = executor.deferred.size() - 1;
    deferred_slot_t& first = executor.deferred[head & mask];
    if (std::atomic_ref<std::uint32_t> {first.ready}.load(std::memory_order_acquire) != head + 1) return 0;
    std::uint32_t const first_ticket = first.ticket;
    std::uint32_t count = 1;
    for (; count != executor.deferred_limit; ++count) {
        deferred_slot_t& slot = executor.deferred[(head + count) & mask];
        if (std::atomic_ref<std::uint32_t> {slot.ready}.load(std::memory_order_acquire) != head + count + 1 ||
            slot.ticket != first_ticket + count)
            break;
    }

    wait_finished(executor, first_ticket);
    auto serve_task = [&executor, head, mask](std::size_t task, std::size_t thread) noexcept {
        deferred_slot_t& slot = executor.deferred[(head + task) & mask];
        run_as(executor.runners[thread],
               [&](ustore_arena_t scratch) noexcept { slot.run(&slot.arguments, executor.results[task], scratch); });
    };
    // A lent pool running its lender's dispatch refuses ours and runs nothing, so the driver runs the batch itself
    if (st::failed(view_of(executor.pool).for_n_dynamic(count, serve_task)))
        for (std::uint32_t task = 0; task != count; ++task) serve_task(task, executor.runners.size() - 1);
    std::atomic_ref<std::uint32_t> {executor.head}.store(head + count, std::memory_order_release);
    publish_results(executor, count);
    advance_finished(executor, first_ticket + count);
    return count;
}

/** Serves the deferred table of @p executor on its driver thread until @c ustore_executor_free stops it. */
void drive(call_executor_t& executor) noexcept {
    std::atomic_ref<std::uint32_t> const doorbell {executor.doorbell};
    for (;;) {
        std::uint32_t const rung = doorbell.load(std::memory_order_acquire);
        if (serve_batch(executor) != 0) continue;
        if (std::atomic_ref<driver_state_t> {executor.state}.load(std::memory_order_acquire) ==
            driver_state_t::stopping_k)
            return;
        if (std::atomic_ref<ustore_mood_t> {executor.mood}.load(std::memory_order_relaxed) == ustore_mood_spinning_k)
            continue;
        if (executor_t* const owned = std::get_if<executor_t>(&executor.pool)) owned->park(parked_check_micros_k);
        doorbell.wait(rung, std::memory_order_acquire);
    }
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

template void route_init(ustore_database_init_ram_t*,
                         status_t (*)(ustore_database_init_ram_t const&, view_t const&) noexcept) noexcept;
template void route_init(ustore_database_open_ram_t*,
                         status_t (*)(ustore_database_open_ram_t const&, view_t const&) noexcept) noexcept;
template void route_init(ustore_database_open_rocksdb_t*,
                         status_t (*)(ustore_database_open_rocksdb_t const&, view_t const&) noexcept) noexcept;
template void route_init(ustore_database_view_rocksdb_t*,
                         status_t (*)(ustore_database_view_rocksdb_t const&, view_t const&) noexcept) noexcept;
template void route_init(ustore_database_open_leveldb_t*,
                         status_t (*)(ustore_database_open_leveldb_t const&, view_t const&) noexcept) noexcept;

/** How a pool spawned here treats the thread spawning it: apart from its workers, or as its only thread in a build
 *  without OS threads. */
caller_exclusivity_t spawned_exclusivity() noexcept {
    return executor_t::spawns_threads() ? caller_exclusivity_t::caller_exclusive_k
                                        : caller_exclusivity_t::caller_inclusive_k;
}

/** Explains why an executor could not have its threads. */
status_t pool_failure(st::status_t status) noexcept {
    switch (status) {
    case st::status_t::out_of_memory_heap_k: return {status_code_t::out_of_memory_k, "Out of memory for the threads"};
    case st::status_t::invalid_argument_k: return {status_code_t::invalid_argument_k, "The pool holds no threads"};
    case st::status_t::operation_not_permitted_k:
        return {status_code_t::unsupported_k, "The system refused to start the threads"};
    default: return {status_code_t::internal_k, "ForkUnion failed for an unknown reason"};
    }
}

/** Reads into @p domains the memory domains @p options name, refusing one @p topology lacks, or every domain the
 *  process may allocate on when they name none. */
status_t read_memory_domains(ustore_ram_options_t const& options, topology_t const& topology,
                             st::basic_vector<fu_memory_domain_id_t>& domains) noexcept {
    std::size_t const available = topology.memory_domains_count();
    bool const derived = !options.memory_domains || !options.memory_domains_count;
    std::size_t const count = derived ? available : static_cast<std::size_t>(options.memory_domains_count);
    if (!count) return {status_code_t::unsupported_k, "The machine reports no memory domain"};
    if (st::failed(domains.resize(count))) return {status_code_t::out_of_memory_k, "Failed to copy the memory domains"};
    for (std::size_t index = 0; index != count; ++index) {
        if (derived) {
            domains[index] = topology.memory_domain(index);
            continue;
        }
        fu_memory_domain_id_t const named = options.memory_domains[index];
        bool known = false;
        for (std::size_t candidate = 0; candidate != available && !known; ++candidate)
            known = topology.memory_domain(candidate) == named;
        if (!known) return {status_code_t::invalid_argument_k, "No memory domain carries the identifier"};
        domains[index] = named;
    }
    return {};
}

/** Whether @p domains hold @p domain. */
bool serves(std::span<fu_memory_domain_id_t const> domains, fu_memory_domain_id_t domain) noexcept {
    return std::find(domains.begin(), domains.end(), domain) != domains.end();
}

/** Counts the cores of the compute domains of @p topology local to any of @p domains. */
std::size_t cores_near(topology_t const& topology, std::span<fu_memory_domain_id_t const> domains) noexcept {
    std::size_t cores = 0;
    for (std::size_t compute_domain = 0; compute_domain != topology.compute_domains_count(); ++compute_domain)
        if (serves(domains, topology.memory_domain_near(compute_domain))) cores += topology.cores_in(compute_domain);
    return cores;
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

/** Copies @p bytes into @p copy, answering whether they fit in memory. */
bool copy_bytes(ustore_bytes_view_t bytes, sz::string_t& copy) noexcept {
    return copy.try_assign(sz::string_view_t {string_of(bytes)}) == sz::status_t::success_k;
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

/** Makes in @p made the executor running calls on @p pool, with its driver and a table of @p deferred_limit calls,
 *  keeping the scratch of every thread of the pool on the memory domain of @p topology local to it when @p domains
 *  hold it, and every other scratch and arena on the first of them. */
static status_t make_executor(std::variant<executor_t, lent_pool_t> pool, topology_t const& topology,
                              std::span<fu_memory_domain_id_t const> domains, std::size_t deferred_limit,
                              ustore_executor_t& made) noexcept {
    std::size_t const limit = deferred_limit ? deferred_limit : deferred_limit_default_k;
    if (limit > deferred_limit_max_k)
        return {status_code_t::capacity_exhausted_k, "An executor holds at most 2^30 deferred calls"};
    st::expected<executor_t> driver = executor_t::make(1, spawned_exclusivity());
    if (!driver) return pool_failure(driver.status());
    std::size_t const threads = view_of(pool).threads_count();
    ustore_executor_handle_t* const executor = new (std::nothrow)
        ustore_executor_handle_t {{std::move(pool), std::move(*driver)}};
    if (!executor) return {status_code_t::out_of_memory_k, "Failed to allocate the executor"};
    executor->deferred_limit = static_cast<std::uint32_t>(limit);
    if (st::failed(executor->deferred.resize(std::bit_ceil(limit))) || st::failed(executor->results.resize(limit)) ||
        st::failed(executor->runners.resize(threads + 1))) {
        delete executor;
        return {status_code_t::out_of_memory_k, "Failed to allocate the deferred table"};
    }

    executor->memory_domain = domains.front();
    for (runner_t& runner : executor->runners) runner.scratch.memory_domain = domains.front();
    // A lent pool running its lender's dispatch refuses this one, leaving every scratch on the first domain
    [[maybe_unused]] st::status_t const placed =
        view_of(executor->pool).for_threads([&](std::size_t thread, std::size_t compute_domain) noexcept {
            fu_memory_domain_id_t const local = topology.memory_domain_near(compute_domain);
            if (serves(domains, local)) executor->runners[thread].scratch.memory_domain = local;
        });

    if (!executor_t::spawns_threads()) executor->state = driver_state_t::absent_k;
    else if (st::expected<fu_generation_t> started = executor->driver.start<&drive>(*executor); started)
        executor->generation = *started;
    else {
        delete executor;
        return pool_failure(started.status());
    }
    made = executor;
    return {};
}

void ustore_database_describe(ustore_database_describe_t* arguments) { route<&engine_t::database_describe>(arguments); }
void ustore_database_clear(ustore_database_clear_t* arguments) { route<&engine_t::database_clear>(arguments); }
void ustore_database_compact(ustore_database_compact_t* arguments) { route<&engine_t::database_compact>(arguments); }
void ustore_snapshot_init(ustore_snapshot_init_t* arguments) { route<&engine_t::snapshot_init>(arguments); }
void ustore_snapshot_save(ustore_snapshot_save_t* arguments) { route<&engine_t::snapshot_save>(arguments); }
void ustore_collection_list(ustore_collection_list_t* arguments) { route<&engine_t::collection_list>(arguments); }
void ustore_collection_create(ustore_collection_create_t* arguments) { route<&engine_t::collection_create>(arguments); }
void ustore_collection_find(ustore_collection_find_t* arguments) { route<&engine_t::collection_find>(arguments); }
void ustore_collection_drop(ustore_collection_drop_t* arguments) { route<&engine_t::collection_drop>(arguments); }
void ustore_transaction_init(ustore_transaction_init_t* arguments) { route<&engine_t::transaction_init>(arguments); }
void ustore_transaction_stage(ustore_transaction_stage_t* arguments) { route<&engine_t::transaction_stage>(arguments); }
void ustore_transaction_commit(ustore_transaction_commit_t* arguments) {
    route<&engine_t::transaction_commit>(arguments);
}
void ustore_transaction_rollback(ustore_transaction_rollback_t* arguments) {
    route<&engine_t::transaction_rollback>(arguments);
}
void ustore_blobs_write(ustore_blobs_write_t* arguments) { route<&engine_t::blobs_write>(arguments); }
void ustore_blobs_remove(ustore_blobs_remove_t* arguments) { route<&engine_t::blobs_remove>(arguments); }
void ustore_blobs_read(ustore_blobs_read_t* arguments) { route<&engine_t::blobs_read>(arguments); }
void ustore_blobs_scan(ustore_blobs_scan_t* arguments) { route<&engine_t::blobs_scan>(arguments); }
void ustore_blobs_sample(ustore_blobs_sample_t* arguments) { route<&engine_t::blobs_sample>(arguments); }
void ustore_blobs_measure(ustore_blobs_measure_t* arguments) { route<&engine_t::blobs_measure>(arguments); }
void ustore_docs_write(ustore_docs_write_t* arguments) { route<&engine_t::docs_write>(arguments); }
void ustore_docs_read(ustore_docs_read_t* arguments) { route<&engine_t::docs_read>(arguments); }
void ustore_docs_gist(ustore_docs_gist_t* arguments) { route<&engine_t::docs_gist>(arguments); }
void ustore_docs_gather(ustore_docs_gather_t* arguments) { route<&engine_t::docs_gather>(arguments); }
void ustore_graph_read_edges(ustore_graph_read_edges_t* arguments) { route<&engine_t::graph_read_edges>(arguments); }
void ustore_graph_write_edges(ustore_graph_write_edges_t* arguments) { route<&engine_t::graph_write_edges>(arguments); }
void ustore_graph_remove_edges(ustore_graph_remove_edges_t* arguments) {
    route<&engine_t::graph_remove_edges>(arguments);
}
void ustore_graph_write_vertices(ustore_graph_write_vertices_t* arguments) {
    route<&engine_t::graph_write_vertices>(arguments);
}
void ustore_graph_remove_vertices(ustore_graph_remove_vertices_t* arguments) {
    route<&engine_t::graph_remove_vertices>(arguments);
}
void ustore_pages_import(ustore_pages_import_t* arguments) { route<&engine_t::pages_import>(arguments); }
void ustore_pages_insert(ustore_pages_insert_t* arguments) { route<&engine_t::pages_insert>(arguments); }
void ustore_pages_remove(ustore_pages_remove_t* arguments) { route<&engine_t::pages_remove>(arguments); }
void ustore_pages_move(ustore_pages_move_t* arguments) { route<&engine_t::pages_move>(arguments); }
void ustore_pages_splice(ustore_pages_splice_t* arguments) { route<&engine_t::pages_splice>(arguments); }
void ustore_pages_set_attributes(ustore_pages_set_attributes_t* arguments) {
    route<&engine_t::pages_set_attributes>(arguments);
}
void ustore_pages_remove_attributes(ustore_pages_remove_attributes_t* arguments) {
    route<&engine_t::pages_remove_attributes>(arguments);
}
void ustore_pages_read(ustore_pages_read_t* arguments) { route<&engine_t::pages_read>(arguments); }
void ustore_pages_history(ustore_pages_history_t* arguments) { route<&engine_t::pages_history>(arguments); }
void ustore_strings_write(ustore_strings_write_t* arguments) { route<&engine_t::strings_write>(arguments); }
void ustore_strings_remove(ustore_strings_remove_t* arguments) { route<&engine_t::strings_remove>(arguments); }
void ustore_strings_read(ustore_strings_read_t* arguments) { route<&engine_t::strings_read>(arguments); }
void ustore_strings_scan(ustore_strings_scan_t* arguments) { route<&engine_t::strings_scan>(arguments); }
void ustore_blobs_cursor_next(ustore_blobs_cursor_next_t* arguments) { route<&engine_t::blobs_scan>(arguments); }
void ustore_strings_cursor_next(ustore_strings_cursor_next_t* arguments) { route<&engine_t::strings_scan>(arguments); }

void ustore_blobs_cursor_init(ustore_blobs_cursor_init_t* arguments) {
    auto [cursor, status] = open_cursor<ustore_blobs_cursor_handle_t, &engine_t::blobs_scan>(*arguments);
    if (status.failed()) return finish(*arguments->outcome, status);
    cursor->start_key = arguments->start_key;
    *arguments->cursor = cursor;
    finish(*arguments->outcome, {});
}

void ustore_strings_cursor_init(ustore_strings_cursor_init_t* arguments) {
    auto [cursor, status] = open_cursor<ustore_strings_cursor_handle_t, &engine_t::strings_scan>(*arguments);
    if (status.failed()) return finish(*arguments->outcome, status);
    if (!copy_bytes(arguments->prefix, cursor->prefix) || !copy_bytes(arguments->start_string, cursor->start)) {
        free_cursor(cursor);
        return finish(*arguments->outcome,
                      {status_code_t::out_of_memory_k, "Failed to copy the prefix and the start of the walk"});
    }
    *arguments->cursor = cursor;
    finish(*arguments->outcome, {});
}

void ustore_blobs_cursor_free(ustore_blobs_cursor_t cursor) { free_cursor(cursor); }
void ustore_strings_cursor_free(ustore_strings_cursor_t cursor) { free_cursor(cursor); }

ustore_view_t ustore_transaction_view(ustore_transaction_t transaction, ustore_watch_t watch) {
    return tagged_view(transaction, watch == ustore_watch_none_k ? view_tag_t::blind_k : view_tag_t::watching_k);
}

ustore_view_t ustore_snapshot_view(ustore_snapshot_t snapshot) { return tagged_view(snapshot, view_tag_t::snapshot_k); }

ustore_status_t ustore_outcome_wait(ustore_outcome_t* outcome) {
    std::atomic_ref<ustore_status_t> const word {outcome->status};
    for (;;) {
        ustore_status_t const seen = word.load(std::memory_order_acquire);
        if (seen != ustore_status_deferred_k) return seen;
        word.wait(seen, std::memory_order_acquire);
    }
}

ustore_status_t ustore_outcome_status(ustore_outcome_t* outcome) {
    return std::atomic_ref<ustore_status_t> {outcome->status}.load(std::memory_order_acquire);
}

char const* ustore_status_name(ustore_status_t status) {
    // A C caller may pass any integer, which C++ may not load as the enum, so its bits are read instead
    std::size_t const index = std::bit_cast<std::underlying_type_t<ustore_status_t>>(status);
    return index < status_names_k.size() ? status_names_k[index].data() : "unrecognized";
}

ustore_status_t ustore_status_from_name(ustore_bytes_view_t name) {
    std::string_view const text {reinterpret_cast<char const*>(name.data), name.length};
    for (std::size_t index = 0; index != status_names_k.size(); ++index)
        if (status_names_k[index] == text) return static_cast<ustore_status_t>(index);
    return ustore_status_invalid_argument_k;
}

char const* ustore_mood_name(ustore_mood_t mood) { return name_in(mood_names_k, mood); }
ustore_mood_t ustore_mood_from_name(ustore_bytes_view_t name) { return value_in(mood_names_k, name); }
char const* ustore_durability_name(ustore_durability_t durability) { return name_in(durability_names_k, durability); }
ustore_durability_t ustore_durability_from_name(ustore_bytes_view_t name) { return value_in(durability_names_k, name); }
char const* ustore_watch_name(ustore_watch_t watch) { return name_in(watch_names_k, watch); }
ustore_watch_t ustore_watch_from_name(ustore_bytes_view_t name) { return value_in(watch_names_k, name); }
char const* ustore_scan_mode_name(ustore_scan_mode_t scan_mode) { return name_in(scan_mode_names_k, scan_mode); }
ustore_scan_mode_t ustore_scan_mode_from_name(ustore_bytes_view_t name) { return value_in(scan_mode_names_k, name); }
char const* ustore_drop_mode_name(ustore_drop_mode_t drop_mode) { return name_in(drop_mode_names_k, drop_mode); }
ustore_drop_mode_t ustore_drop_mode_from_name(ustore_bytes_view_t name) { return value_in(drop_mode_names_k, name); }
char const* ustore_isolation_name(ustore_isolation_t isolation) { return name_in(isolation_names_k, isolation); }
ustore_isolation_t ustore_isolation_from_name(ustore_bytes_view_t name) { return value_in(isolation_names_k, name); }
char const* ustore_transport_name(ustore_transport_t transport) { return name_in(transport_names_k, transport); }
ustore_transport_t ustore_transport_from_name(ustore_bytes_view_t name) { return value_in(transport_names_k, name); }
char const* ustore_engine_name(ustore_engine_t engine) { return name_in(engine_names_k, engine); }
ustore_engine_t ustore_engine_from_name(ustore_bytes_view_t name) { return value_in(engine_names_k, name); }
char const* ustore_modality_name(ustore_modality_t modality) { return name_in(modality_names_k, modality); }
ustore_modality_t ustore_modality_from_name(ustore_bytes_view_t name) { return value_in(modality_names_k, name); }

char const* ustore_vertex_role_name(ustore_vertex_role_t vertex_role) {
    return name_in(vertex_role_names_k, vertex_role);
}

ustore_vertex_role_t ustore_vertex_role_from_name(ustore_bytes_view_t name) {
    return value_in(vertex_role_names_k, name);
}

char const* ustore_docs_field_type_name(ustore_docs_field_type_t field_type) {
    return name_in(docs_field_type_names_k, field_type);
}

ustore_docs_field_type_t ustore_docs_field_type_from_name(ustore_bytes_view_t name) {
    return value_in(docs_field_type_names_k, name);
}

char const* ustore_docs_modification_name(ustore_docs_modification_t modification) {
    return name_in(docs_modification_names_k, modification);
}

ustore_docs_modification_t ustore_docs_modification_from_name(ustore_bytes_view_t name) {
    return value_in(docs_modification_names_k, name);
}

char const* ustore_page_encoding_name(ustore_page_encoding_t encoding) {
    return name_in(page_encoding_names_k, encoding);
}

ustore_page_encoding_t ustore_page_encoding_from_name(ustore_bytes_view_t name) {
    return value_in(page_encoding_names_k, name);
}

char const* ustore_page_modification_name(ustore_page_modification_t modification) {
    return name_in(page_modification_names_k, modification);
}

ustore_page_modification_t ustore_page_modification_from_name(ustore_bytes_view_t name) {
    return value_in(page_modification_names_k, name);
}

void ustore_version(int* major, int* minor, int* patch) {
    *major = USTORE_VERSION_MAJOR;
    *minor = USTORE_VERSION_MINOR;
    *patch = USTORE_VERSION_PATCH;
}

char const* ustore_version_name(void) { return version_name_k.data(); }

void ustore_executor_for_ram(ustore_executor_for_ram_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    if (!arguments->executor)
        return finish(outcome, {status_code_t::invalid_argument_k, "The call needs an output for the executor"});
    *arguments->executor = nullptr;
    st::expected<topology_t> topology = topology_t::make();
    if (!topology) return finish(outcome, pool_failure(topology.status()));
    st::basic_vector<fu_memory_domain_id_t> domains;
    if (status_t const status = read_memory_domains(arguments->ram, *topology, domains); status.failed())
        return finish(outcome, status);
    std::size_t threads = arguments->threads;
    if (!executor_t::spawns_threads()) {
        if (threads > 1) return finish(outcome, {status_code_t::unsupported_k, "This build spawns no threads"});
        threads = 1;
    }
    else if (!threads) threads = std::max<std::size_t>(cores_near(*topology, domains) * 7 / 8, 1);
    // ForkUnion spawns near one memory domain or across every one, so naming several but not all takes the machine
    bool const near_one = domains.size() == 1 && topology->memory_domains_count() > 1;
    st::expected<executor_t> pool = near_one
                                        ? executor_t::make_near(*topology, domains[0], threads, spawned_exclusivity())
                                        : executor_t::make(*topology, threads, spawned_exclusivity());
    if (!pool) return finish(outcome, pool_failure(pool.status()));
    finish(outcome,
           make_executor(std::move(*pool), *topology, domains, arguments->deferred_limit, *arguments->executor));
}

void ustore_executor_on_pool(ustore_executor_on_pool_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    if (!arguments->executor)
        return finish(outcome, {status_code_t::invalid_argument_k, "The call needs an output for the executor"});
    *arguments->executor = nullptr;
    st::expected<lent_pool_t> pool = lent_pool_t::borrow(arguments->pool);
    if (!pool) return finish(outcome, pool_failure(pool.status()));
    st::expected<topology_t> topology = topology_t::make();
    if (!topology) return finish(outcome, pool_failure(topology.status()));
    st::basic_vector<fu_memory_domain_id_t> domains;
    if (status_t const status = read_memory_domains(arguments->ram, *topology, domains); status.failed())
        return finish(outcome, status);
    finish(outcome, make_executor(*pool, *topology, domains, arguments->deferred_limit, *arguments->executor));
}

void ustore_executor_for_nvme(ustore_executor_for_nvme_t* arguments) {
    clear(arguments->executor);
    finish(*arguments->outcome, {status_code_t::unsupported_k, "This build has no NVMe executor"});
}

void ustore_executor_drain(ustore_executor_drain_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    ustore_executor_handle_t* const executor = arguments->executor;
    if (arguments->mood != ustore_mood_parked_k && arguments->mood != ustore_mood_spinning_k)
        return finish(outcome, {status_code_t::invalid_argument_k, "The mood is neither parked nor spinning"});
    if (!executor) return finish(outcome, {});
    if (runs_here(*executor))
        return finish(outcome, {status_code_t::contended_k, "A callback cannot drain the executor running it"});
    std::atomic_ref<ustore_mood_t> {executor->mood}.store(arguments->mood, std::memory_order_relaxed);
    wait_finished(*executor,
                  tickets_of(std::atomic_ref<std::uint64_t> {executor->submitted}.load(std::memory_order_acquire)));
    finish(outcome, {});
}

void ustore_executor_free(ustore_executor_t executor) {
    if (!executor) return;
    wait_finished(*executor,
                  tickets_of(std::atomic_ref<std::uint64_t> {executor->submitted}.load(std::memory_order_acquire)));
    if (executor->state == driver_state_t::serving_k) {
        std::atomic_ref<driver_state_t> {executor->state}.store(driver_state_t::stopping_k, std::memory_order_release);
        std::atomic_ref<std::uint32_t> const doorbell {executor->doorbell};
        doorbell.fetch_add(1, std::memory_order_release);
        doorbell.notify_all();
        executor->driver.join(executor->generation);
    }
    for (runner_t& runner : executor->runners) arena_free_blocks(runner.scratch);
    delete executor;
}

void ustore_transaction_free(ustore_transaction_t transaction) {
    if (!transaction) return;
    wait_deferred_calls(handle_cast<handle_t>(transaction)->deferred_calls);
    release_parent(view_t {.transaction = transaction});
}

void ustore_snapshot_free(ustore_snapshot_t snapshot) {
    if (!snapshot) return;
    wait_deferred_calls(handle_cast<handle_t>(snapshot)->deferred_calls);
    release_parent(view_t {.snapshot = snapshot});
}

void ustore_database_free(ustore_database_t database) {
    if (!database) return;
    wait_deferred_calls(handle_cast<handle_t>(database)->deferred_calls);
    release_hold(database);
}

void ustore_arena_init(ustore_arena_init_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    if (!arguments->arena)
        return finish(outcome, {status_code_t::invalid_argument_k, "The call needs an output for the arena"});
    *arguments->arena = nullptr;
    // Every residency this build computes in places its arenas on host pages, which the first call allocates
    ustore_arena_t const arena = new (std::nothrow) ustore_arena_handle_t {};
    if (!arena) return finish(outcome, {status_code_t::out_of_memory_k, "Failed to allocate the arena"});
    arena->residency = residency_of(arguments->executor);
    if (arguments->executor) arena->memory_domain = arguments->executor->memory_domain;
    *arguments->arena = arena;
    finish(outcome, {});
}

void ustore_arena_free(ustore_arena_t arena) {
    if (!arena) return;
    wait_deferred_calls(arena->deferred_calls);
    arena_free(arena);
}

#if !USTORE_WITH_ROCKSDB
void ustore_database_open_rocksdb(ustore_database_open_rocksdb_t* arguments) {
    finish(*arguments->outcome, {status_code_t::unsupported_k, "This build carries no RocksDB engine"});
}

void ustore_database_view_rocksdb(ustore_database_view_rocksdb_t* arguments) {
    finish(*arguments->outcome, {status_code_t::unsupported_k, "This build carries no RocksDB engine"});
}
#endif

#if !USTORE_WITH_LEVELDB
void ustore_database_open_leveldb(ustore_database_open_leveldb_t* arguments) {
    finish(*arguments->outcome, {status_code_t::unsupported_k, "This build carries no LevelDB engine"});
}
#endif

#if !USTORE_WITH_NVME
void ustore_database_open_nvme(ustore_database_open_nvme_t* arguments) {
    finish(*arguments->outcome, {status_code_t::unsupported_k, "This build carries no NVMe engine"});
}
#endif
