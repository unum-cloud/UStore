/**
 *  @file src/lib.hpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief The router every exported call of the C ABI goes through, to the engine behind its database or transaction
 *      handle, shared by the one source per public header.
 *
 *  Each engine defines only its own @c ustore_database_init_*, @c ustore_database_open_* and
 *  @c ustore_database_view_*, which stamp every handle they hand out with its @c engine_t, so one
 *  library holds every engine it was built with and a program links only those it opens. The opens
 *  of an engine the build lacks are defined in <tt>src/db.cpp</tt> and refuse, so every program links
 *  against every build, and every other one names its engine's open as the template argument of
 *  @c route_init, which routes it as any call. Every call of the table is @c noexcept, so nothing
 *  here catches. The router claims the arena and the transaction of a call, refuses a change to a
 *  database opened read-only, then runs the call on the calling thread, blocking on its executor,
 *  or queues it in the executor's deferred table, and finishes its outcome when the engine returns.
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
#include <cstdint> // `std::uint32_t`, `std::uint64_t`, `std::uintptr_t`

#include <algorithm>   // `std::min`
#include <array>       // `std::array`
#include <bit>         // `std::bit_cast`
#include <memory>      // `std::addressof`
#include <new>         // `std::nothrow`, placement `new`
#include <numeric>     // `std::iota`
#include <optional>    // `std::optional`
#include <string_view> // `std::string_view`
#include <thread>      // `std::this_thread::get_id`, `std::thread::id`
#include <tuple>       // `std::apply`
#include <type_traits> // `std::underlying_type_t`, `std::is_same_v`
#include <variant>     // `std::variant`, `std::visit`, `std::get_if`

#include <stringzilla/stringzilla.hpp> // `sz::string_t`

#include "ustore/utilities/ram_buffers.hpp" // `arena_rewind`, `arena_page_bytes_k`
#include "ustore/utilities/ram_device.hpp"  // `executor_t`, `lent_pool_t`, `claim_t`

#include "engine.hpp"            // `engine_t`, `ustore_*_handle_t`, `view_t`, `host_verbs`, `floor_atomic_ref`
#include "match.hpp"             // `compiled_match`, `compile_match`, `scan_matched`, `exhausted_t`
#include "utilities/batches.hpp" // `strings_arg_t`
#include "vram/matching.hpp"     // `compile_match`, `scan_matched_on_device`
#include "vram/staging.hpp"      // `stage_inputs`

namespace unum::ustore {
using namespace unum::ustore::utilities;

/** What every cursor holds: the claim of the pull filling it, its deferred pulls, and what it walks. */
struct cursor_t {

    /** Whether a pull fills the cursor, so a second pull naming it finds it contended. */
    claim_t claim = claim_t::free_k;

    /** Deferred pulls naming the cursor that are not final, through the floor class's reference. */
    std::uint32_t deferred_calls = 0;

    /** Where the walk's match was compiled once, freed with the cursor. */
    ustore_arena_handle_t compiled {};

    /** The database the walk reads, held open until the cursor is freed. */
    ustore_database_t database = nullptr;

    /** What the walk sees, whose transaction or snapshot the cursor holds until freed. */
    ustore_view_t view = nullptr;

    /** The collection the walk reads. */
    ustore_collection_id_t collection {};

    /** Which way the walk goes from its start. */
    ustore_scan_direction_t direction = ustore_scan_ascending_k;
};

/** The verbs a walk lists and reads through at one home, and the match it compiled once at init. */
template <template <home_t> typename verbs_type_, home_t home_>
struct cursor_engine {
    verbs_type_<home_> const* verbs = nullptr;
    compiled_match<home_> match;
};

/** The engine of a walk at whichever home its init's executor serves. */
template <template <home_t> typename verbs_type_>
using cursor_engines =
    std::variant<cursor_engine<verbs_type_, home_t::host_k>, cursor_engine<verbs_type_, home_t::vram_nvidia_k>,
                 cursor_engine<verbs_type_, home_t::vram_amd_k>>;

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
    std::string_view const text = string_of(name);
    for (enumerator_spelling<enum_type_> const& spelling : names)
        if (spelling.name == text) return spelling.value;
    return enum_type_ {};
}

#pragma endregion Names

#pragma region GPU Classes

/** The class of @p device, read off the architecture its driver reported: an NVIDIA generation without a class of its
 *  own runs as Ampere, one older than Ampere as none, and an AMD processor without a class as none. */
constexpr gpu_class_t gpu_class_of(gpu_device_t const& device) noexcept {
    std::uint32_t const major = device.architecture_major, minor = device.architecture_minor;
    if (device.vendor == gpu_vendor_t::nvidia_k) {
        if (major < 8) return gpu_class_t::unknown_k;
        if (major == 8) return minor >= 9 ? gpu_class_t::nvidia_ada_k : gpu_class_t::nvidia_ampere_k;
        if (major == 9) return gpu_class_t::nvidia_hopper_k;
        if (major == 10) return gpu_class_t::nvidia_blackwell_k;
        if (major == 12) return gpu_class_t::nvidia_blackwellrtx_k;
        return gpu_class_t::nvidia_ampere_k;
    }
    if (device.vendor == gpu_vendor_t::amd_k) {
        if (major == 9 && minor == 4) return gpu_class_t::amd_cdna3_k;
        if (major == 9 && minor == 5) return gpu_class_t::amd_cdna4_k;
        if (major == 12 && minor == 5) return gpu_class_t::amd_cdna5_k;
    }
    return gpu_class_t::unknown_k;
}

/** The GPU side of the process, built at its first VRAM call: every vendor's driver, and the devices some class runs
 *  on, NVIDIA first, then AMD, each opened by the first executor spawned on it. */
struct gpu_runtime_t {
    gpu_drivers_t drivers;
    kernel_tables<cuda_module_t> cuda_kernels;
    kernel_tables<hip_module_t> hip_kernels;
    gpu_device_t devices[2 * gpu_devices_capacity_k];
    std::size_t devices_count = 0;

    /** Taken by the first spawn on every device, NVIDIA ones by CUDA ordinal, then AMD ones
     *  by HIP ordinal. */
    claim_t opening[2 * gpu_devices_capacity_k] = {};

    /** What the open of every device answered, in the order of @c opening, deferred
     *  until it ends. */
    ustore_outcome_t opened[2 * gpu_devices_capacity_k];

    gpu_runtime_t() noexcept {
        std::size_t const reported = list_gpu_devices(drivers, devices);
        for (std::size_t position = 0; position != reported; ++position) {
            gpu_device_t const& device = devices[position];
            if (gpu_class_of(device) == gpu_class_t::unknown_k || device.vendor_ordinal >= gpu_devices_capacity_k)
                continue;
            devices[devices_count++] = device;
        }
        for (ustore_outcome_t& outcome : opened) outcome = {ustore_status_deferred_k, nullptr};
    }

    /** Spawns into @p into an executor on @p device as @c spawn_gpu_executor does, the first spawn
     *  on the device opening it while every other one waits for that open. */
    template <typename variant_type_>
    status_t spawn(gpu_device_t const& device, gpu_class_t gpu_class, void* stream, std::size_t scratch_limit,
                   ustore_gds_t gds, variant_type_& into) noexcept {
        if ((device.vendor != gpu_vendor_t::nvidia_k && device.vendor != gpu_vendor_t::amd_k) ||
            device.vendor_ordinal >= gpu_devices_capacity_k)
            return {status_code_t::device_k, "No driver serves the device"};
        std::size_t const slot = (device.vendor == gpu_vendor_t::amd_k ? gpu_devices_capacity_k : 0) +
                                 device.vendor_ordinal;
        class_verbs_t const& verbs = host_verbs();
        if (verbs.take(&opening[slot])) {
            status_t const status = open_gpu_device(drivers, device, cuda_kernels, hip_kernels);
            opened[slot].message = status.message;
            verbs.publish(&opened[slot], static_cast<ustore_status_t>(status.code), std::memory_order_release);
        }
        if (ustore_status_t const status = verbs.wait(&opened[slot]); status != ustore_status_success_k)
            return {static_cast<status_code_t>(status), opened[slot].message};
        return spawn_gpu_executor(drivers, cuda_kernels, hip_kernels, device, gpu_class, stream, scratch_limit, gds,
                                  into);
    }
};

/** The one runtime every VRAM call passes its drivers down from. */
inline gpu_runtime_t& gpu_runtime() noexcept {
    static gpu_runtime_t runtime;
    return runtime;
}

/** The devices VRAM executors spawn on, a device's position here being its UStore ordinal. */
inline std::span<gpu_device_t const> gpu_devices() noexcept {
    gpu_runtime_t const& runtime = gpu_runtime();
    return {runtime.devices, runtime.devices_count};
}

/** Frees every block of @p arena wherever it lives, a device's runs through the process's drivers as the executor that
 *  made the arena may be gone, and the arena itself, where null is safe. */
inline void destroy_arena(ustore_arena_t arena) noexcept {
    if (!arena) return;
    if (cuda_blocks_t* const memory = std::get_if<cuda_blocks_t>(&arena->memory)) {
        gpu_drivers_t const& drivers = gpu_runtime().drivers;
        free_cuda_blocks(drivers.cuda, drivers.cuda_contexts[memory->device], memory->blocks);
    }
    else if (hip_blocks_t* const memory = std::get_if<hip_blocks_t>(&arena->memory))
        free_hip_blocks(gpu_runtime().drivers.hip, memory->device, memory->blocks);
    else arena_free_blocks(*arena);
    delete arena;
}

#pragma endregion GPU Classes

/** The tag @c ustore_transaction_view and @c ustore_snapshot_view set in the low bits of a handle. */
enum class view_tag_t : std::uintptr_t {
    head_k = 0,
    watching_k = 1,
    blind_k = 2,
    snapshot_k = 3,
};

/** The low bits of a handle a view tag takes, which every handle's alignment leaves free. */
constexpr std::uintptr_t view_tag_mask_k = 3;
static_assert(alignof(ustore_transaction_handle_t) > view_tag_mask_k &&
              alignof(ustore_snapshot_handle_t) > view_tag_mask_k);

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

/** Decodes what @p arguments read through into @p view, answering false for a view neither view
 *  constructor made. A pull reads through the view its cursor opened with. */
template <typename arguments_type_>
bool decode_call(arguments_type_ const& arguments, view_t& view) noexcept {
    if constexpr (requires { arguments.cursor->cursor.view; }) return decode_view(arguments.cursor->cursor.view, view);
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
template <typename handle_type_>
void take_hold(handle_type_* handle) noexcept {
    floor_atomic_ref<std::uint32_t> {handle->holds}.fetch_add(1, std::memory_order_relaxed);
}

/** Drops one hold on @p handle, answering whether it was the last. */
template <typename handle_type_>
bool drop_hold(handle_type_* handle) noexcept {
    floor_atomic_ref<std::uint32_t> const holds {handle->holds};
    return holds.fetch_sub(1, std::memory_order_acq_rel) == 1;
}

/** Keeps @p database open until @p opened, a transaction or a snapshot of it, is freed. */
template <typename handle_type_>
void hold(ustore_database_t database, handle_type_* opened) noexcept {
    opened->database = database;
    take_hold(database);
}

/** Drops one hold on @p database, if any, closing it through its engine with the last. */
inline void release_hold(ustore_database_t database) noexcept {
    if (!database || !drop_hold(database)) return;
    engine_of(database).database->database_free(database);
    delete database;
}

/** Drops one hold on @p parent, a transaction or a snapshot, if any, freeing it through its engine
 *  with the last, which gives back the hold it kept on its database. */
template <typename handle_type_>
void release_parent(handle_type_* parent) noexcept {
    if (!parent || !drop_hold(parent)) return;
    ustore_database_t const database = parent->database;
    if constexpr (std::is_same_v<handle_type_, ustore_transaction_handle_t>)
        engine_of(parent).database->transaction_free(parent);
    else engine_of(parent).database->snapshot_free(parent);
    delete parent;
    release_hold(database);
}

/** Whether @p candidate_type_ is one of @p listed_types_. */
template <typename candidate_type_, typename... listed_types_>
constexpr bool one_of = (std::is_same_v<candidate_type_, listed_types_> || ...);

/** Whether a call changes its database, which one opened read-only refuses: every call naming a
 *  transaction, as reads name views instead, and the calls on whole collections. */
template <typename arguments_type_>
constexpr bool changes_database = requires(arguments_type_ const& arguments) { arguments.transaction; } ||
                                  one_of<arguments_type_, ustore_collection_create_t, ustore_collection_drop_t,
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
    clear(arguments.offsets), clear(arguments.keys), clear(arguments.scores);
}

inline void clear_outputs(ustore_blobs_sample_t const& arguments) noexcept {
    clear(arguments.offsets), clear(arguments.keys);
}

/** Clears the six bounds of a measure of any modality. */
template <typename arguments_type_>
    requires requires(arguments_type_ const& arguments) { arguments.max_space_usages; }
void clear_outputs(arguments_type_ const& arguments) noexcept {
    clear(arguments.min_cardinalities), clear(arguments.max_cardinalities), clear(arguments.min_value_bytes);
    clear(arguments.max_value_bytes), clear(arguments.min_space_usages), clear(arguments.max_space_usages);
}

inline void clear_outputs(ustore_strings_sample_t const& arguments) noexcept {
    clear(arguments.offsets), clear(arguments.strings);
}

inline void clear_outputs(ustore_graph_sample_vertices_t const& arguments) noexcept {
    clear(arguments.offsets), clear(arguments.keys);
}

inline void clear_outputs(ustore_pages_sample_t const& arguments) noexcept {
    clear(arguments.offsets), clear(arguments.keys);
}

inline void clear_outputs(ustore_docs_read_t const& arguments) noexcept {
    clear(arguments.presences), clear(arguments.lengths), clear(arguments.values);
}

inline void clear_outputs(ustore_docs_gist_t const& arguments) noexcept {
    clear(arguments.fields), clear(arguments.written);
}

inline void clear_outputs(ustore_docs_gather_t const& arguments) noexcept { clear(arguments.columns); }

inline void clear_outputs(ustore_docs_scan_t const& arguments) noexcept {
    clear(arguments.offsets), clear(arguments.keys);
}

inline void clear_outputs(ustore_docs_sample_t const& arguments) noexcept {
    clear(arguments.offsets), clear(arguments.keys);
}

inline void clear_outputs(ustore_graph_read_edges_t const& arguments) noexcept {
    clear(arguments.degrees_per_vertex), clear(arguments.edges_offsets), clear(arguments.edges_per_vertex);
}

inline void clear_outputs(ustore_graph_scan_vertices_t const& arguments) noexcept {
    clear(arguments.offsets), clear(arguments.keys);
}

inline void clear_outputs(ustore_pages_scan_t const& arguments) noexcept {
    clear(arguments.offsets), clear(arguments.keys);
}

inline void clear_outputs(ustore_pages_insert_t const& arguments) noexcept { clear(arguments.effective_keys); }

inline void clear_outputs(ustore_pages_read_t const& arguments) noexcept {
    clear(arguments.presences), clear(arguments.lengths), clear(arguments.values);
}

inline void clear_outputs(ustore_strings_read_t const& arguments) noexcept {
    clear(arguments.presences), clear(arguments.values);
}

inline void clear_outputs(ustore_strings_scan_t const& arguments) noexcept {
    clear(arguments.offsets), clear(arguments.strings), clear(arguments.scores);
}

inline void clear_outputs(ustore_blobs_cursor_next_t const& arguments) noexcept {
    clear(arguments.keys), clear(arguments.values), clear(arguments.written);
}

inline void clear_outputs(ustore_strings_cursor_next_t const& arguments) noexcept {
    clear(arguments.strings), clear(arguments.values), clear(arguments.written);
}

inline void clear_outputs(ustore_docs_cursor_next_t const& arguments) noexcept {
    clear(arguments.keys), clear(arguments.values), clear(arguments.written);
}

inline void clear_outputs(ustore_pages_cursor_next_t const& arguments) noexcept {
    clear(arguments.keys), clear(arguments.values), clear(arguments.written);
}

inline void clear_outputs(ustore_graph_vertices_cursor_next_t const& arguments) noexcept {
    clear(arguments.keys), clear(arguments.written);
}

inline void clear_outputs(ustore_changes_cursor_init_t const& arguments) noexcept { clear(arguments.cursor); }

inline void clear_outputs(ustore_changes_cursor_next_t const& arguments) noexcept {
    clear(arguments.count), clear(arguments.keys), clear(arguments.stamps), clear(arguments.kinds);
    clear(arguments.values);
}

#pragma endregion Outputs

#pragma region Ragged Inputs

/** A ragged input of a call and the number of strings it holds. */
struct ragged_input_t {
    ustore_strings_view_t view;
    std::size_t count;

    ragged_input_t(ustore_strings_view_t view, ustore_size_t count) noexcept
        : view(view), count(static_cast<std::size_t>(count)) {}
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

inline std::array<ragged_input_t, 1> ragged_inputs(ustore_strings_write_t const& arguments) noexcept {
    return {{{arguments.strings, arguments.count}}};
}

inline std::array<ragged_input_t, 2> ragged_inputs(ustore_strings_remove_t const& arguments) noexcept {
    return {{{arguments.strings, arguments.count}, {arguments.end_strings, arguments.count}}};
}

inline std::array<ragged_input_t, 1> ragged_inputs(ustore_strings_read_t const& arguments) noexcept {
    return {{{arguments.strings, arguments.count}}};
}

inline std::array<ragged_input_t, 3> ragged_inputs(ustore_strings_scan_t const& arguments) noexcept {
    return {{{arguments.prefixes, arguments.count},
             {arguments.start_strings, arguments.count},
             {arguments.end_strings, arguments.count}}};
}

inline std::array<ragged_input_t, 2> ragged_inputs(ustore_strings_measure_t const& arguments) noexcept {
    return {{{arguments.start_strings, arguments.count}, {arguments.end_strings, arguments.count}}};
}

/** Strings an accessor writes per block, so the indices it reads fit on the stack of the thread running the call. */
constexpr std::size_t accessor_block_k = 64;

/** Reads @p input for its engine into @p strings, refusing a layout no constant names and gathering
 *  into @p scratch the views an accessor writes, so the accessor runs on the thread running the
 *  call and never after it is final. */
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
 *  The engine's scan lists every page into the arena, which the match narrows, until the survivors fill the page or
 *  the collection ends; then the arena is rewound, the read writes the values of the survivors there when asked, and
 *  the keys follow them, so the arena holds only the outputs. The cursor advances only once the progress callback let
 *  the pull finish.
 */
status_t pull(engine_t const& engine, ustore_blobs_cursor_next_t const& arguments, ustore_blobs_cursor_handle_t& cursor,
              view_t const& view) noexcept;

/** Pulls the next page of a walk over documents, as the blobs pull does, reading whole documents when asked. */
status_t pull(engine_t const& engine, ustore_docs_cursor_next_t const& arguments, ustore_docs_cursor_handle_t& cursor,
              view_t const& view) noexcept;

/** Pulls the next page of a walk over pages, as the blobs pull does, rendering whole pages when asked. */
status_t pull(engine_t const& engine, ustore_pages_cursor_next_t const& arguments, ustore_pages_cursor_handle_t& cursor,
              view_t const& view) noexcept;

/** Pulls the next page of a walk over vertices, as the blobs pull does. */
status_t pull(engine_t const& engine, ustore_graph_vertices_cursor_next_t const& arguments,
              ustore_graph_vertices_cursor_handle_t& cursor, view_t const& view) noexcept;

/** Pulls the next page of the walk @p arguments name through @p engine, as the blobs pull does,
 *  resuming past the last string at that string followed by a zero byte, the smallest string after
 *  it. */
status_t pull(engine_t const& engine, ustore_strings_cursor_next_t const& arguments,
              ustore_strings_cursor_handle_t& cursor, view_t const& view) noexcept;

/** The bodies of the pulls above, narrowing through @p match, the walk's own or the face a device
 *  pull shows it, each defined beside its pull. */
template <typename match_type_>
status_t pull_through(engine_t const& engine, ustore_blobs_cursor_next_t const& arguments,
                      ustore_blobs_cursor_handle_t& cursor, match_type_ const& match, view_t const& view) noexcept;
template <typename match_type_>
status_t pull_through(engine_t const& engine, ustore_docs_cursor_next_t const& arguments,
                      ustore_docs_cursor_handle_t& cursor, match_type_ const& match, view_t const& view) noexcept;
template <typename match_type_>
status_t pull_through(engine_t const& engine, ustore_pages_cursor_next_t const& arguments,
                      ustore_pages_cursor_handle_t& cursor, match_type_ const& match, view_t const& view) noexcept;
template <typename match_type_>
status_t pull_through(engine_t const& engine, ustore_graph_vertices_cursor_next_t const& arguments,
                      ustore_graph_vertices_cursor_handle_t& cursor, match_type_ const& match,
                      view_t const& view) noexcept;
template <typename match_type_>
status_t pull_through(engine_t const& engine, ustore_strings_cursor_next_t const& arguments,
                      ustore_strings_cursor_handle_t& cursor, match_type_ const& match, view_t const& view) noexcept;

/** Opens the change feed @p arguments ask for through @p engine, which pins its stamp, holding the
 *  database open until the feed is freed. */
status_t open_changes_cursor(engine_t const& engine, ustore_changes_cursor_init_t const& arguments,
                             view_t const& view) noexcept;

/** Pulls the next changes of the feed @p arguments name through @p engine. */
status_t pull(engine_t const& engine, ustore_changes_cursor_next_t const& arguments,
              ustore_changes_cursor_handle_t& cursor, view_t const& view) noexcept;

/** Refuses a feed pulled on a device, as change feeds read host runs alone. */
template <gpu_executor_concept executor_type_>
status_t pull(engine_t const&, ustore_changes_cursor_next_t const&, ustore_changes_cursor_handle_t&, view_t const&,
              executor_type_&) noexcept {
    return {status_code_t::unsupported_k, "Change feeds run on host executors"};
}

/** The host engine of @p cursor, or null for one opened for a device. */
template <typename cursor_type_>
auto host_engine_of(cursor_type_& cursor) noexcept {
    return std::get_if<0>(&cursor.engine);
}

/**
 *  @brief Pulls the next page of the walk @p cursor computes on @p executor's device, as
 *      @c scan_matched_on_device serves a filtered scan.
 *
 *  The host lists every page into the executor's bounce, the device matches its values, and every
 *  output moves into the call's device arena once. The cursor's engine compiled at its init into
 *  the cursor's own arena, which outlives every call, and each pull rebases it onto a copy of its
 *  tables in the managed runs the call rewinds. A cursor opened on the host walks there alone. The
 *  pull body of every walk, @c pull_through, is defined where its modality's pull is.
 */
template <typename arguments_type_, typename cursor_type_, gpu_executor_concept executor_type_>
status_t pull(engine_t const& engine, arguments_type_ const& arguments, cursor_type_& cursor, view_t const& view,
              executor_type_& executor) noexcept {
    constexpr home_t home_k = home_of_executors<executor_type_>;
    constexpr std::size_t home_index_k = home_k == home_t::vram_nvidia_k ? 1 : 2;
    auto const* const walker = std::get_if<home_index_k>(&cursor.engine);
    if (!walker) return {status_code_t::device_k, "The cursor walks on another kind of executor"};
    device_blocks_t* const blocks = device_blocks_of(arguments.arena, executor);
    if (!blocks) return {status_code_t::device_k, "The arena lives where another executor computes"};
    compiled_match<home_k> match = walker->match;
    if (match.reads_values() && !executor.kernels())
        return {status_code_t::unsupported_k, "The device loaded no bundle of the library's kernels"};
    if (status_t const status = rebase_on_device(match, executor); status.failed()) return status;
    device_match<home_k> const matching {match, executor, *blocks};
    arguments_type_ hosted = arguments;
    hosted.arena = executor.bounce();
    if (status_t const status = pull_through(engine, hosted, cursor, matching, view); status.failed()) return status;
    return upload_output_columns(hosted, *blocks, executor);
}

/** The keys @p cursor walks, pulled into @p survivors through @p engine and @p match, the walk's
 *  own or its device face, as one page of @p wanted, advancing @p resume past them, for every
 *  pull over keys. */
template <typename scan_type_, typename cursor_type_, typename match_type_>
status_t list_cursor_keys(engine_t const& engine, ustore_database_t database, cursor_type_& cursor,
                          match_type_ const& match, std::size_t wanted, ustore_arena_t arena, view_t const& view,
                          keyed_survivors_t& survivors, std::variant<ustore_key_t, exhausted_t>& resume) noexcept {
    resume = cursor.position;
    ustore_key_t const* const start = std::get_if<ustore_key_t>(&cursor.position);
    if (!start || !wanted) return {};
    return list_matched_keys<scan_type_>(engine.host, database, cursor.cursor.collection, *start,
                                         cursor.end_key ? std::addressof(*cursor.end_key) : nullptr,
                                         cursor.cursor.direction, wanted, match, arena, view, survivors, resume);
}

#pragma endregion Cursors

/** Finishes a call: the message, then the status with release ordering, which its waiters
 *  watch by address. */
inline void finish(ustore_outcome_t& outcome, status_t status) noexcept {
    outcome.message = status.message;
    host_verbs().publish(&outcome, static_cast<ustore_status_t>(status.code), std::memory_order_release);
}

#pragma region Claims

/** Takes @p word for one call, answering whether no other call held it. */
inline bool take(claim_t& word) noexcept { return host_verbs().take(&word); }

/** Hands @p word back to the next call. */
inline void release(claim_t& word) noexcept { host_verbs().release(&word); }

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

/** The deferred count of the cursor @p arguments pull, or of the transaction or snapshot they read
 *  through, or null for none, as the cursor's holds keep what it reads through alive. */
template <typename arguments_type_>
std::uint32_t* named_calls_of(arguments_type_ const& arguments) noexcept {
    if constexpr (requires { arguments.cursor->cursor; }) return &arguments.cursor->cursor.deferred_calls;
    else {
        view_t view;
        decode_call(arguments, view);
        if (view.transaction) return &view.transaction->deferred_calls;
        return view.snapshot ? &view.snapshot->deferred_calls : nullptr;
    }
}

/** The deferred counts of the database, the transaction, snapshot or cursor, and the arena
 *  @p arguments name, each null when absent. */
template <typename arguments_type_>
std::array<std::uint32_t*, 3> deferred_counts_of(arguments_type_ const& arguments) noexcept {
    ustore_arena_t const arena = arena_of(arguments);
    ustore_database_t const database = database_of(arguments);
    return {database ? &database->deferred_calls : nullptr, named_calls_of(arguments),
            arena ? &arena->deferred_calls : nullptr};
}

/** An init names no handle yet, only an output for the database it opens, so it counts nowhere. */
template <typename arguments_type_>
    requires one_of<arguments_type_, ustore_database_init_ram_t, ustore_database_open_ram_t,
                    ustore_database_open_rocksdb_t, ustore_database_view_rocksdb_t, ustore_database_open_leveldb_t,
                    ustore_database_open_nvme_t>
std::array<std::uint32_t*, 3> deferred_counts_of(arguments_type_ const&) noexcept {
    return {};
}

/** The claim of the cursor @p arguments pull, or null for a call naming none. */
template <typename arguments_type_>
claim_t* cursor_claim_of(arguments_type_ const& arguments) noexcept {
    if constexpr (requires { arguments.cursor->cursor; }) return &arguments.cursor->cursor.claim;
    else return nullptr;
}

/** The home of the calls on @p executor, where a null one runs them on the calling thread over host pages. */
inline home_t home_of(ustore_executor_handle_t const* executor) noexcept {
    if (!executor) return home_t::host_k;
    if (std::holds_alternative<cuda_executor_t>(executor->device)) return home_t::vram_nvidia_k;
    if (std::holds_alternative<hip_executor_t>(executor->device)) return home_t::vram_amd_k;
    return home_t::host_k;
}

/** Whether @p arena lives where calls on @p executor compute: host pages for host executors, the
 *  device of a VRAM executor for its calls. */
inline bool arena_serves(ustore_executor_handle_t const* executor, ustore_arena_t arena) noexcept {
    if (!executor) return std::holds_alternative<host_blocks_t>(arena->memory);
    return std::visit(
        [&]<typename device_type_>(device_type_ const& device) noexcept {
            if constexpr (gpu_executor_concept<device_type_>)
                return utilities::device_blocks_of(arena, device) != nullptr;
            else return std::holds_alternative<host_blocks_t>(arena->memory);
        },
        executor->device);
}

/** Claims the arena, the cursor and the transaction of @p arguments, holding none when one serves
 *  another call, and refusing a call that needs an arena or a cursor and names none, or whose arena
 *  lives where its executor does not compute. */
template <typename arguments_type_>
status_t take_claims(arguments_type_ const& arguments) noexcept {
    if constexpr (requires { arguments.cursor; })
        if (!arguments.cursor) return {status_code_t::invalid_argument_k, "The call needs a cursor"};
    ustore_arena_t const arena = arena_of(arguments);
    if constexpr (requires { arguments.arena; }) {
        if (!arena) return {status_code_t::invalid_argument_k, "The call needs an arena for its outputs"};
        if (!arena_serves(arguments.executor, arena))
            return {status_code_t::device_k, "The arena lives where another executor computes"};
        if (!take(arena->claim)) return {status_code_t::contended_k, "The arena serves another call"};
        if (floor_atomic_ref<std::uint32_t> {arena->exports}.load(std::memory_order_seq_cst) != 0) {
            release(arena->claim);
            return {status_code_t::contended_k, "Arrow still reads the arena's outputs"};
        }
    }
    claim_t* const cursor = cursor_claim_of(arguments);
    if (cursor && !take(*cursor)) {
        if (arena) release(arena->claim);
        return {status_code_t::contended_k, "The cursor serves another pull"};
    }
    ustore_transaction_t const transaction = transaction_of(arguments);
    if (transaction && !take(transaction->claim)) {
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
    if (ustore_transaction_t const transaction = transaction_of(arguments)) release(transaction->claim);
}

/** Finishes @p arguments with @p status without serving them, releasing the claims the router took. */
template <typename arguments_type_>
void refuse(arguments_type_& arguments, status_t status) noexcept {
    release_claims(arguments);
    finish(*arguments.outcome, status);
}

/** Waits until no deferred call counted in @p calls is left, so the handle counting them may be freed. */
inline void wait_deferred_calls(std::uint32_t& calls) noexcept {
    floor_atomic_ref<std::uint32_t const> const counted {calls};
    for (std::uint32_t seen = counted.load(std::memory_order_acquire); seen != 0;
         seen = counted.load(std::memory_order_acquire))
        host_verbs().await_change(&calls, seen);
}

#pragma endregion Claims

#pragma region Verb Slots

/**
 *  @brief Where the verb serving an argument structure sits: @c table names the modality's table at
 *      a home and @c verb the verb in it, each a variable template over the home, or @c database
 *      the verb of the control plane, which serves on host executors alone.
 *
 *  A @c verb whose home lacks the slot fails its constraint, as every change does on a device.
 */
template <typename>
struct verb_slot;

template <>
struct verb_slot<ustore_database_describe_t> {
    static constexpr auto database = &database_verbs_t::describe;
};
template <>
struct verb_slot<ustore_database_clear_t> {
    static constexpr auto database = &database_verbs_t::clear;
};
template <>
struct verb_slot<ustore_database_compact_t> {
    static constexpr auto database = &database_verbs_t::compact;
};
template <>
struct verb_slot<ustore_snapshot_init_t> {
    static constexpr auto database = &database_verbs_t::snapshot_init;
};
template <>
struct verb_slot<ustore_snapshot_save_t> {
    static constexpr auto database = &database_verbs_t::snapshot_save;
};
template <>
struct verb_slot<ustore_collection_list_t> {
    static constexpr auto database = &database_verbs_t::collection_list;
};
template <>
struct verb_slot<ustore_collection_create_t> {
    static constexpr auto database = &database_verbs_t::collection_create;
};
template <>
struct verb_slot<ustore_collection_find_t> {
    static constexpr auto database = &database_verbs_t::collection_find;
};
template <>
struct verb_slot<ustore_collection_drop_t> {
    static constexpr auto database = &database_verbs_t::collection_drop;
};
template <>
struct verb_slot<ustore_transaction_init_t> {
    static constexpr auto database = &database_verbs_t::transaction_init;
};
template <>
struct verb_slot<ustore_transaction_stage_t> {
    static constexpr auto database = &database_verbs_t::transaction_stage;
};
template <>
struct verb_slot<ustore_transaction_commit_t> {
    static constexpr auto database = &database_verbs_t::transaction_commit;
};
template <>
struct verb_slot<ustore_transaction_rollback_t> {
    static constexpr auto database = &database_verbs_t::transaction_rollback;
};
template <>
struct verb_slot<ustore_changes_cursor_init_t> {
    static constexpr auto database = &database_verbs_t::changes_cursor_init;
};
template <>
struct verb_slot<ustore_blobs_write_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::blobs;
    template <home_t home_>
        requires requires { &blobs_verbs<home_>::write; }
    static constexpr auto verb = &blobs_verbs<home_>::write;
};
template <>
struct verb_slot<ustore_blobs_remove_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::blobs;
    template <home_t home_>
        requires requires { &blobs_verbs<home_>::remove; }
    static constexpr auto verb = &blobs_verbs<home_>::remove;
};
template <>
struct verb_slot<ustore_blobs_read_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::blobs;
    template <home_t home_>
        requires requires { &blobs_verbs<home_>::read; }
    static constexpr auto verb = &blobs_verbs<home_>::read;
};
template <>
struct verb_slot<ustore_blobs_scan_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::blobs;
    template <home_t home_>
        requires requires { &blobs_verbs<home_>::scan; }
    static constexpr auto verb = &blobs_verbs<home_>::scan;
};
template <>
struct verb_slot<ustore_blobs_sample_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::blobs;
    template <home_t home_>
        requires requires { &blobs_verbs<home_>::sample; }
    static constexpr auto verb = &blobs_verbs<home_>::sample;
};
template <>
struct verb_slot<ustore_blobs_measure_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::blobs;
    template <home_t home_>
        requires requires { &blobs_verbs<home_>::measure; }
    static constexpr auto verb = &blobs_verbs<home_>::measure;
};
template <>
struct verb_slot<ustore_strings_write_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::strings;
    template <home_t home_>
        requires requires { &strings_verbs<home_>::write; }
    static constexpr auto verb = &strings_verbs<home_>::write;
};
template <>
struct verb_slot<ustore_strings_remove_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::strings;
    template <home_t home_>
        requires requires { &strings_verbs<home_>::remove; }
    static constexpr auto verb = &strings_verbs<home_>::remove;
};
template <>
struct verb_slot<ustore_strings_read_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::strings;
    template <home_t home_>
        requires requires { &strings_verbs<home_>::read; }
    static constexpr auto verb = &strings_verbs<home_>::read;
};
template <>
struct verb_slot<ustore_strings_scan_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::strings;
    template <home_t home_>
        requires requires { &strings_verbs<home_>::scan; }
    static constexpr auto verb = &strings_verbs<home_>::scan;
};
template <>
struct verb_slot<ustore_docs_write_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::docs;
    template <home_t home_>
        requires requires { &docs_verbs<home_>::write; }
    static constexpr auto verb = &docs_verbs<home_>::write;
};
template <>
struct verb_slot<ustore_docs_read_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::docs;
    template <home_t home_>
        requires requires { &docs_verbs<home_>::read; }
    static constexpr auto verb = &docs_verbs<home_>::read;
};
template <>
struct verb_slot<ustore_docs_gist_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::docs;
    template <home_t home_>
        requires requires { &docs_verbs<home_>::gist; }
    static constexpr auto verb = &docs_verbs<home_>::gist;
};
template <>
struct verb_slot<ustore_docs_gather_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::docs;
    template <home_t home_>
        requires requires { &docs_verbs<home_>::gather; }
    static constexpr auto verb = &docs_verbs<home_>::gather;
};
template <>
struct verb_slot<ustore_docs_remove_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::docs;
    template <home_t home_>
        requires requires { &docs_verbs<home_>::remove; }
    static constexpr auto verb = &docs_verbs<home_>::remove;
};
template <>
struct verb_slot<ustore_docs_scan_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::docs;
    template <home_t home_>
        requires requires { &docs_verbs<home_>::scan; }
    static constexpr auto verb = &docs_verbs<home_>::scan;
};
template <>
struct verb_slot<ustore_docs_sample_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::docs;
    template <home_t home_>
        requires requires { &docs_verbs<home_>::sample; }
    static constexpr auto verb = &docs_verbs<home_>::sample;
};
template <>
struct verb_slot<ustore_graph_read_edges_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::graph;
    template <home_t home_>
        requires requires { &graph_verbs<home_>::read_edges; }
    static constexpr auto verb = &graph_verbs<home_>::read_edges;
};
template <>
struct verb_slot<ustore_graph_write_edges_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::graph;
    template <home_t home_>
        requires requires { &graph_verbs<home_>::write_edges; }
    static constexpr auto verb = &graph_verbs<home_>::write_edges;
};
template <>
struct verb_slot<ustore_graph_remove_edges_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::graph;
    template <home_t home_>
        requires requires { &graph_verbs<home_>::remove_edges; }
    static constexpr auto verb = &graph_verbs<home_>::remove_edges;
};
template <>
struct verb_slot<ustore_graph_write_vertices_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::graph;
    template <home_t home_>
        requires requires { &graph_verbs<home_>::write_vertices; }
    static constexpr auto verb = &graph_verbs<home_>::write_vertices;
};
template <>
struct verb_slot<ustore_graph_remove_vertices_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::graph;
    template <home_t home_>
        requires requires { &graph_verbs<home_>::remove_vertices; }
    static constexpr auto verb = &graph_verbs<home_>::remove_vertices;
};
template <>
struct verb_slot<ustore_graph_scan_vertices_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::graph;
    template <home_t home_>
        requires requires { &graph_verbs<home_>::scan_vertices; }
    static constexpr auto verb = &graph_verbs<home_>::scan_vertices;
};
template <>
struct verb_slot<ustore_pages_import_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::pages;
    template <home_t home_>
        requires requires { &pages_verbs<home_>::import; }
    static constexpr auto verb = &pages_verbs<home_>::import;
};
template <>
struct verb_slot<ustore_pages_insert_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::pages;
    template <home_t home_>
        requires requires { &pages_verbs<home_>::insert; }
    static constexpr auto verb = &pages_verbs<home_>::insert;
};
template <>
struct verb_slot<ustore_pages_remove_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::pages;
    template <home_t home_>
        requires requires { &pages_verbs<home_>::remove; }
    static constexpr auto verb = &pages_verbs<home_>::remove;
};
template <>
struct verb_slot<ustore_pages_move_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::pages;
    template <home_t home_>
        requires requires { &pages_verbs<home_>::move; }
    static constexpr auto verb = &pages_verbs<home_>::move;
};
template <>
struct verb_slot<ustore_pages_splice_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::pages;
    template <home_t home_>
        requires requires { &pages_verbs<home_>::splice; }
    static constexpr auto verb = &pages_verbs<home_>::splice;
};
template <>
struct verb_slot<ustore_pages_set_attributes_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::pages;
    template <home_t home_>
        requires requires { &pages_verbs<home_>::set_attributes; }
    static constexpr auto verb = &pages_verbs<home_>::set_attributes;
};
template <>
struct verb_slot<ustore_pages_remove_attributes_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::pages;
    template <home_t home_>
        requires requires { &pages_verbs<home_>::remove_attributes; }
    static constexpr auto verb = &pages_verbs<home_>::remove_attributes;
};
template <>
struct verb_slot<ustore_pages_read_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::pages;
    template <home_t home_>
        requires requires { &pages_verbs<home_>::read; }
    static constexpr auto verb = &pages_verbs<home_>::read;
};
template <>
struct verb_slot<ustore_pages_scan_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::pages;
    template <home_t home_>
        requires requires { &pages_verbs<home_>::scan; }
    static constexpr auto verb = &pages_verbs<home_>::scan;
};
template <>
struct verb_slot<ustore_strings_sample_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::strings;
    template <home_t home_>
        requires requires { &strings_verbs<home_>::sample; }
    static constexpr auto verb = &strings_verbs<home_>::sample;
};
template <>
struct verb_slot<ustore_strings_measure_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::strings;
    template <home_t home_>
        requires requires { &strings_verbs<home_>::measure; }
    static constexpr auto verb = &strings_verbs<home_>::measure;
};
template <>
struct verb_slot<ustore_docs_measure_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::docs;
    template <home_t home_>
        requires requires { &docs_verbs<home_>::measure; }
    static constexpr auto verb = &docs_verbs<home_>::measure;
};
template <>
struct verb_slot<ustore_graph_sample_vertices_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::graph;
    template <home_t home_>
        requires requires { &graph_verbs<home_>::sample_vertices; }
    static constexpr auto verb = &graph_verbs<home_>::sample_vertices;
};
template <>
struct verb_slot<ustore_graph_measure_vertices_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::graph;
    template <home_t home_>
        requires requires { &graph_verbs<home_>::measure_vertices; }
    static constexpr auto verb = &graph_verbs<home_>::measure_vertices;
};
template <>
struct verb_slot<ustore_pages_sample_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::pages;
    template <home_t home_>
        requires requires { &pages_verbs<home_>::sample; }
    static constexpr auto verb = &pages_verbs<home_>::sample;
};
template <>
struct verb_slot<ustore_pages_measure_t> {
    template <home_t home_>
    static constexpr auto table = &home_tables<home_>::pages;
    template <home_t home_>
        requires requires { &pages_verbs<home_>::measure; }
    static constexpr auto verb = &pages_verbs<home_>::measure;
};

#pragma endregion Verb Slots

#pragma region Serving

/** Hands @p visitor the device executor of @p view, as <tt>visitor(device)</tt> for a VRAM executor
 *  and <tt>visitor()</tt> for every host one, so a verb takes its home's executor as its last
 *  argument. */
template <typename visitor_type_>
status_t visit_home(view_t const& view, visitor_type_&& visitor) noexcept {
    ustore_executor_t const executor = view.executor;
    if (cuda_executor_t* const device = executor ? std::get_if<cuda_executor_t>(&executor->device) : nullptr)
        return visitor(*device);
    if (hip_executor_t* const device = executor ? std::get_if<hip_executor_t>(&executor->device) : nullptr)
        return visitor(*device);
    return visitor();
}

/** Calls @p verb on @p arguments with every ragged input gathered on the host into the scratch
 *  @p view works in, then @p view and the executor of the verb's home. */
template <typename verb_type_, typename arguments_type_, typename... executors_types_>
status_t call_gathered(verb_type_ verb, arguments_type_ const& arguments, view_t const& view,
                       executors_types_&... executor) noexcept {
    if constexpr (requires { ragged_inputs(arguments); }) {
        auto const inputs = ragged_inputs(arguments);
        std::array<strings_arg_t, std::tuple_size_v<decltype(inputs)>> strings;
        for (std::size_t index = 0; index != inputs.size(); ++index)
            if (status_t const status = gather_strings(inputs[index], view.scratch, strings[index]); status.failed())
                return status;
        return std::apply(
            [&](auto const&... gathered) noexcept { return verb(arguments, gathered..., view, executor...); }, strings);
    }
    else return verb(arguments, view, executor...);
}

/** Calls @p verb on @p arguments as @c call_gathered does, moving the inputs of a device call to the host first. */
template <typename verb_type_, typename arguments_type_, typename... executors_types_>
status_t call_staged(verb_type_ verb, arguments_type_ const& arguments, view_t const& view,
                     executors_types_&... executor) noexcept {
    if constexpr (sizeof...(executors_types_) != 0) {
        arguments_type_ staged = arguments;
        if (status_t const status = stage_inputs(gpu_runtime().drivers, executor..., staged); status.failed())
            return status;
        return call_gathered(verb, staged, view, executor...);
    }
    else return call_gathered(verb, arguments, view);
}

/** Readies @p executor for one call of @p arguments: binds its device to the calling thread, rewinds the bounce and
 *  the managed runs, as every call before was waited for, and rewinds the call's arena, freeing its older runs in
 *  stream order. */
template <typename arguments_type_, gpu_executor_concept executor_type_>
status_t open_device_call(arguments_type_ const& arguments, executor_type_& executor) noexcept {
    if (status_t const status = executor.bind(); status.failed()) return status;
    executor.rewind_bounce();
    utilities::rewind(executor.managed(), executor);
    if constexpr (requires { arguments.arena; })
        if (device_blocks_t* const blocks = utilities::device_blocks_of(arguments.arena, executor))
            utilities::rewind(*blocks, executor);
    return {};
}

/** Serves @p arguments on the engine of their database, in the scratch and on the executor thread
 *  @p runner names, through the verb table of its executor's home, refusing what no engine may see,
 *  and handing every ragged input to the engine readable in any layout. A pull serves through its
 *  cursor. */
template <typename arguments_type_>
status_t serve(arguments_type_ const& arguments, view_t const& runner) noexcept {
    ustore_database_t const database = database_of(arguments);
    if (!database) return {status_code_t::uninitialized_k, "The database is uninitialized"};
    engine_t const& engine = engine_of(database);
    if constexpr (changes_database<arguments_type_>)
        if (database->access == access_t::read_only_k)
            return {status_code_t::read_only_k, "The database was opened read-only"};
    if constexpr (requires { arguments.cursor->cursor; })
        if (arguments.cursor->cursor.database != database)
            return {status_code_t::invalid_argument_k, "The cursor walks another database"};
    if constexpr (requires { arguments.collections.data; })
        if (arguments.count && !arguments.collections.data)
            return {status_code_t::invalid_argument_k, "No collections were named"};
    if constexpr (std::is_same_v<arguments_type_, ustore_collection_create_t>) {
        if (arguments.modality < ustore_modality_blobs_k || arguments.modality > ustore_modality_pages_k)
            return {status_code_t::invalid_argument_k, "Unknown modality"};
        if (arguments.name.length && arguments.name.data && arguments.name.data[0] == '_')
            return {status_code_t::invalid_argument_k, "Names starting with _ are reserved for system collections"};
    }

    view_t view = runner;
    if (!decode_call(arguments, view))
        return {status_code_t::invalid_argument_k, "The view was made by neither view constructor"};
    if (view.transaction ? view.transaction->engine != &engine : view.snapshot && view.snapshot->engine != &engine)
        return {status_code_t::invalid_argument_k, "The view belongs to another engine"};

    if constexpr (requires { arguments.arena; }) arena_rewind(arguments.arena);
    arena_rewind(runner.scratch);
    return visit_home(view, [&]<typename... executors_types_>(executors_types_&... executor) noexcept -> status_t {
        constexpr home_t home_k = home_of_executors<executors_types_...>;
        if constexpr (home_k != home_t::host_k)
            if (status_t const opened = open_device_call(arguments, executor...); opened.failed()) return opened;
        if constexpr (requires { arguments.cursor->cursor; })
            return pull(engine, arguments, *arguments.cursor, view, executor...);
        else if constexpr (home_k != home_t::host_k && !requires { verb_slot<arguments_type_>::template verb<home_k>; })
            return status_t {status_code_t::unsupported_k, "Changes and the control plane run on host executors"};
        else if constexpr (requires { verb_slot<arguments_type_>::database; }) {
            auto const verb = engine.database->*verb_slot<arguments_type_>::database;
            if (!verb) return status_t {status_code_t::unsupported_k, "The engine does not implement this call"};
            if constexpr (std::is_same_v<arguments_type_, ustore_transaction_init_t>) {
                ustore_transaction_t const reused = arguments.transaction ? *arguments.transaction : nullptr;
                if (reused && reused->database != arguments.database)
                    return status_t {status_code_t::invalid_argument_k, "The transaction belongs to another database"};
                status_t const status = call_staged(verb, arguments, view);
                if (!status.failed() && !reused) hold(arguments.database, *arguments.transaction);
                return status;
            }
            else if constexpr (std::is_same_v<arguments_type_, ustore_snapshot_init_t>) {
                status_t const status = call_staged(verb, arguments, view);
                if (!status.failed()) hold(arguments.database, *arguments.snapshot);
                return status;
            }
            else if constexpr (std::is_same_v<arguments_type_, ustore_changes_cursor_init_t>)
                return open_changes_cursor(engine, arguments, view);
            else return call_staged(verb, arguments, view);
        }
        else {
            auto const* const verbs = engine.template tables<home_k>().*
                                      verb_slot<arguments_type_>::template table<home_k>;
            if (!verbs)
                return status_t {status_code_t::unsupported_k, "The engine keeps no collections of that modality"};
            auto const verb = verbs->*verb_slot<arguments_type_>::template verb<home_k>;
            if (!verb) return status_t {status_code_t::unsupported_k, "The engine does not implement this call"};
            if constexpr (carries_match<arguments_type_>) {
                auto [match, match_status] = compile_match(arguments, runner.scratch, database->ceiling.strings,
                                                           executor...);
                if (match_status.failed()) return match_status;
                if (!match.matches_everything())
                    return call_staged(
                        [&](arguments_type_ const& matched, auto&... rest) noexcept {
                            if constexpr (home_k == home_t::host_k)
                                return scan_matched(engine.host, match, matched, rest...);
                            else return scan_matched_on_device(engine.host, match, matched, rest...);
                        },
                        arguments, view, executor...);
            }
            return call_staged(verb, arguments, view, executor...);
        }
    });
}

#pragma endregion Serving

#pragma region Routing

/** The tickets taken, in the low half of @c ustore_executor_handle_t::submitted. */
constexpr std::uint32_t tickets_of(std::uint64_t submitted) noexcept { return static_cast<std::uint32_t>(submitted); }

/** The table positions taken, in the high half of @c ustore_executor_handle_t::submitted. */
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
inline void wait_finished(ustore_executor_handle_t& executor, std::uint32_t ticket) noexcept {
    floor_atomic_ref<std::uint32_t const> const finished {executor.finished};
    for (std::uint32_t seen = finished.load(std::memory_order_acquire); !reached(seen, ticket);
         seen = finished.load(std::memory_order_acquire))
        host_verbs().await_change(&executor.finished, seen);
}

/** Marks every ticket below @p ticket final for whoever waits for one. */
inline void advance_finished(ustore_executor_handle_t& executor, std::uint32_t ticket) noexcept {
    floor_atomic_ref<std::uint32_t> {executor.finished}.store(ticket, std::memory_order_release);
}

/** Whether the calling thread runs a call of @p executor, so waiting on the executor would wait on itself. */
inline bool runs_here(ustore_executor_handle_t& executor) noexcept {
    std::thread::id const self = std::this_thread::get_id();
    for (runner_t& runner : executor.runners)
        if (floor_atomic_ref<std::thread::id> {runner.thread}.load(std::memory_order_relaxed) == self) return true;
    return false;
}

/** Runs @p body on the calling thread as runner @p thread_index of @p executor, over its scratch,
 *  recorded there while it does. */
template <typename body_type_>
void run_as(ustore_executor_handle_t& executor, std::size_t thread_index, body_type_&& body) noexcept {
    runner_t& runner = executor.runners[thread_index];
    floor_atomic_ref<std::thread::id> const recorded {runner.thread};
    recorded.store(std::this_thread::get_id(), std::memory_order_relaxed);
    body(view_t {.scratch = &runner.scratch, .executor = &executor, .thread_index = thread_index});
    recorded.store(std::thread::id {}, std::memory_order_relaxed);
}

/** The right to dispatch on @p pool, owned or lent. */
inline pool_view_t view_of(std::variant<executor_t, lent_pool_t> const& pool) noexcept {
    return std::visit([](auto const& alternative) noexcept { return alternative.view(); }, pool);
}

/** Serves @p arguments in the scratch and on the thread @p runner names and releases their claims,
 *  leaving the outputs of a failed call null or zero, and answers the status their outcome is to
 *  finish with. */
template <typename arguments_type_>
status_t serve_and_release(arguments_type_& arguments, view_t const& runner) noexcept {
    status_t const status = serve(arguments, runner);
    if constexpr (requires { clear_outputs(arguments); })
        if (status.failed()) clear_outputs(arguments);
    release_claims(arguments);
    return status;
}

/** Opens the database @p call names through @p open_, its engine's open, which claims nothing, under the ceiling its
 *  pinned class sets, which the opened handle keeps for every later call. */
template <typename arguments_type_, auto open_>
status_t serve_open(arguments_type_& call, view_t const& runner) noexcept {
    ustore_cpu_class_t pinned = ustore_cpu_class_unknown_k;
    if constexpr (requires { call.cpu_class; }) pinned = call.cpu_class;
    auto const [ceiling, pick_status] = pick_cpu_ceiling(pinned);
    if (pick_status.failed()) return pick_status;
    if (status_t const opened = open_(call, ceiling, runner); opened.failed()) return opened;
    (*call.database)->ceiling = ceiling;
    return {};
}

/** Serves @p call with @p serve_ in the scratch and on the thread @p runner names, then finishes its outcome. */
template <typename call_type_, status_t (*serve_)(call_type_&, view_t const&) noexcept>
void run(call_type_* call, view_t const& runner) noexcept {
    finish(*call->outcome, serve_(*call, runner));
}

/** Runs @p call on the calling thread over a scratch made for it alone, as a call without an
 *  executor keeping scratch needs: its first page lies on the stack, so a small call allocates
 *  nothing, and the blocks it grows into are freed once the call is final. */
template <typename call_type_, status_t (*serve_)(call_type_&, view_t const&) noexcept>
void run_alone(call_type_* call) noexcept {
    alignas(arena_alignment_k) byte_t page[arena_page_bytes_k];
    arena_block_t* const first = new (page) arena_block_t {nullptr, sizeof(page), sizeof(arena_block_t)};
    ustore_arena_handle_t scratch {.memory = host_blocks_t {.newest = first}};
    run<call_type_, serve_>(call, view_t {.scratch = &scratch});
    arena_free_blocks(scratch, first);
}

/** Serves the copy @p erased holds on the thread @p runner names into @p result, leaving its
 *  outcome and its deferred counts to the driver, which frees the copy's table position first. */
template <typename call_type_, status_t (*serve_)(call_type_&, view_t const&) noexcept>
void run_deferred(void* erased, deferred_result_t& result, view_t const& runner) noexcept {
    call_type_& call = *std::get_if<call_type_>(static_cast<deferred_arguments_t*>(erased));
    // Counted before the call runs, as a transaction init rewrites the handle its arguments point to
    result.deferred_counts = deferred_counts_of(call);
    result.outcome = call.outcome;
    result.status = serve_(call, runner);
}

/** Runs @p call on the calling thread after every call submitted before it finishes, and before any after. */
template <typename call_type_, status_t (*serve_)(call_type_&, view_t const&) noexcept>
void run_blocking(ustore_executor_handle_t& executor, call_type_* call) noexcept {
    if (runs_here(executor))
        return refuse(*call, {status_code_t::contended_k, "A callback cannot block on the executor running it"});
    floor_atomic_ref<std::uint64_t> const submitted {executor.submitted};
    std::uint64_t observed = submitted.load(std::memory_order_relaxed);
    while (!submitted.compare_exchange_weak(observed, submitted_of(tickets_of(observed) + 1, positions_of(observed)),
                                            std::memory_order_relaxed)) {}
    std::uint32_t const ticket = tickets_of(observed);
    wait_finished(executor, ticket);
    run_as(executor, executor.runners.size() - 1,
           [call](view_t const& runner) noexcept { run<call_type_, serve_>(call, runner); });
    advance_finished(executor, ticket + 1);
}

/** Copies @p call into the deferred table of @p executor, finishing it contended when the table is full. */
template <typename call_type_, status_t (*serve_)(call_type_&, view_t const&) noexcept>
void submit(ustore_executor_handle_t& executor, call_type_* call) noexcept {
    if (floor_atomic_ref<driver_state_t> {executor.state}.load(std::memory_order_relaxed) == driver_state_t::absent_k)
        return run_alone<call_type_, serve_>(call);

    host_verbs().publish(call->outcome, ustore_status_deferred_k, std::memory_order_relaxed);
    floor_atomic_ref<std::uint64_t> const submitted {executor.submitted};
    floor_atomic_ref<std::uint32_t const> const head {executor.head};
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
        if (count) floor_atomic_ref<std::uint32_t> {*count}.fetch_add(1, std::memory_order_relaxed);
    deferred_slot_t& slot = executor.deferred[position & (executor.deferred.size() - 1)];
    slot.arguments.template emplace<call_type_>(*call);
    slot.run = &run_deferred<call_type_, serve_>;
    slot.ticket = tickets_of(observed);
    floor_atomic_ref<std::uint32_t> {slot.ready}.store(position + 1, std::memory_order_release);
    floor_atomic_ref<std::uint32_t> {executor.doorbell}.fetch_add(1, std::memory_order_release);
}

/** Runs @p call on the calling thread, blocking on its executor, or submits it, as its executor and completion ask. */
template <typename call_type_, status_t (*serve_)(call_type_&, view_t const&) noexcept>
void dispatch(call_type_* call) noexcept {
    ustore_executor_handle_t* const executor = call->executor;
    if (!executor) return run_alone<call_type_, serve_>(call);
    if (call->completion == ustore_completion_blocking_k) return run_blocking<call_type_, serve_>(*executor, call);
    submit<call_type_, serve_>(*executor, call);
}

/** Leaves the outputs of @p arguments null or zero and claims what they hold, then dispatches them
 *  to the verb their type names. */
template <typename arguments_type_>
void route(arguments_type_* arguments) noexcept {
    if constexpr (requires { clear_outputs(*arguments); }) clear_outputs(*arguments);
    if (status_t const claimed = take_claims(*arguments); claimed.failed()) return finish(*arguments->outcome, claimed);
    dispatch<arguments_type_, &serve_and_release<arguments_type_>>(arguments);
}

/**
 *  @brief Routes the init @p arguments of an engine as every call is routed: on the calling thread, blocking on their
 *      executor, or submitted to it, served by @p open_, the engine's open, since no handle names the engine yet.
 *
 *  Called by the inits, opens and views of the in-memory, RocksDB, LevelDB and NVMe engines, each
 *  naming its own @p open_, so a program still links only the engines it opens.
 */
template <auto open_, typename arguments_type_>
void route_init(arguments_type_* arguments) noexcept {
    if (arguments->database) *arguments->database = nullptr;
    if (home_of(arguments->executor) != home_t::host_k)
        return finish(*arguments->outcome, {status_code_t::device_k, "Databases open on host executors"});
    dispatch<arguments_type_, &serve_open<arguments_type_, open_>>(arguments);
}

#pragma endregion Routing

#pragma region Cursor Lifetimes

/** Opens the engine of a walk over @p scan_type_ at @p home_, compiling what @p arguments match
 *  once into @p compiled. */
template <home_t home_, template <home_t> typename verbs_type_, typename scan_type_, typename arguments_type_>
expected<cursor_engines<verbs_type_>> open_cursor_home(engine_t const& engine, arguments_type_ const& arguments,
                                                       ustore_arena_t compiled) noexcept {
    verbs_type_<home_> const* const verbs = engine.template tables<home_>().*
                                            verb_slot<scan_type_>::template table<home_>;
    if (!verbs || !(verbs->*verb_slot<scan_type_>::template verb<home_>))
        return status_t {status_code_t::unsupported_k, "The engine keeps no collections of that modality"};
    auto [match, match_status] = compile_match(arguments, compiled, arguments.database->ceiling.strings);
    if (match_status.failed()) return match_status;
    if constexpr (home_ == home_t::host_k)
        return cursor_engines<verbs_type_> {cursor_engine<verbs_type_, home_> {verbs, match}};
    else {
        // A walk outlives the call opening it, so its engine stays in the cursor's arena, and every
        // pull rebases it onto the managed runs that call rewinds
        compiled_match<home_> on_device {match.matcher, match};
        on_device.host.matcher = match_everything_t {};
        return cursor_engines<verbs_type_> {cursor_engine<verbs_type_, home_> {verbs, on_device}};
    }
}

/** Opens the engine of a walk over @p scan_type_ at the home of @p executor. */
template <template <home_t> typename verbs_type_, typename scan_type_, typename arguments_type_>
expected<cursor_engines<verbs_type_>> open_cursor_engine(engine_t const& engine,
                                                         ustore_executor_handle_t const* executor,
                                                         arguments_type_ const& arguments,
                                                         ustore_arena_t compiled) noexcept {
    switch (home_of(executor)) {
    case home_t::vram_nvidia_k:
        return open_cursor_home<home_t::vram_nvidia_k, verbs_type_, scan_type_>(engine, arguments, compiled);
    case home_t::vram_amd_k:
        return open_cursor_home<home_t::vram_amd_k, verbs_type_, scan_type_>(engine, arguments, compiled);
    default: return open_cursor_home<home_t::host_k, verbs_type_, scan_type_>(engine, arguments, compiled);
    }
}

/** Opens a cursor of @p cursor_type_ over the walk @p arguments describe, holding their database
 *  and the transaction or snapshot their view reads through, at their executor's home, and refusing
 *  a database whose engine lacks the verb serving @p scan_type_ there, a match that does not
 *  compile, or a view of another database. */
template <typename cursor_type_, template <home_t> typename verbs_type_, typename scan_type_, typename arguments_type_>
expected<cursor_type_*> open_cursor(arguments_type_ const& arguments) noexcept {
    if (!arguments.cursor)
        return status_t {status_code_t::invalid_argument_k, "The call needs an output for the cursor"};
    *arguments.cursor = nullptr;
    ustore_database_t const database = arguments.database;
    if (!database) return status_t {status_code_t::uninitialized_k, "The database is uninitialized"};
    if (status_t const status = validate_direction(arguments.direction); status.failed()) return status;
    view_t view;
    if (!decode_view(arguments.view, view))
        return status_t {status_code_t::invalid_argument_k, "The view was made by neither view constructor"};
    if (view.transaction ? view.transaction->database != database
                         : view.snapshot && view.snapshot->database != database)
        return status_t {status_code_t::invalid_argument_k, "The view reads another database"};

    cursor_type_* const cursor = new (std::nothrow) cursor_type_ {};
    if (!cursor) return status_t {status_code_t::out_of_memory_k, "Failed to allocate the cursor"};
    auto [opened, opened_status] = open_cursor_engine<verbs_type_, scan_type_>(engine_of(database), arguments.executor,
                                                                               arguments, &cursor->cursor.compiled);
    if (opened_status.failed()) {
        arena_free_blocks(cursor->cursor.compiled);
        delete cursor;
        return opened_status;
    }
    cursor->engine = opened;
    cursor->cursor.database = database;
    cursor->cursor.view = arguments.view;
    cursor->cursor.collection = arguments.collection;
    cursor->cursor.direction = arguments.direction;
    if constexpr (requires { arguments.end_key; })
        if (arguments.end_key) cursor->end_key = *arguments.end_key;
    take_hold(database);
    if (view.transaction) take_hold(view.transaction);
    if (view.snapshot) take_hold(view.snapshot);
    return cursor;
}

/** Waits until no deferred pull of @p cursor is left, then gives back its holds and frees it, where null is safe. */
template <typename cursor_type_>
void free_cursor(cursor_type_* cursor) noexcept {
    if (!cursor) return;
    wait_deferred_calls(cursor->cursor.deferred_calls);
    view_t view;
    decode_view(cursor->cursor.view, view);
    release_parent(view.transaction);
    release_parent(view.snapshot);
    release_hold(cursor->cursor.database);
    arena_free_blocks(cursor->cursor.compiled);
    delete cursor;
}

#pragma endregion Cursor Lifetimes

} // namespace unum::ustore

using namespace unum::ustore;

/** What @c ustore_blobs_cursor_t points to: a walk over the keys of one blobs collection, resumed
 *  at the key after the last one it handed out. */
struct ustore_blobs_cursor_handle_t {
    cursor_t cursor;

    /** The first key in the walk's direction the next pull may hand out, or the end of the walk. */
    std::variant<ustore_key_t, exhausted_t> position;

    /** The key the walk stops before, if any. */
    std::optional<ustore_key_t> end_key;

    /** The verbs and the match of the home the walk computes at. */
    cursor_engines<blobs_verbs> engine;
};

/** What @c ustore_docs_cursor_t points to: a walk over the keys of one documents collection,
 *  resumed as the blobs walk is. */
struct ustore_docs_cursor_handle_t {
    cursor_t cursor;

    /** The first key in the walk's direction the next pull may hand out, or the end of the walk. */
    std::variant<ustore_key_t, exhausted_t> position;

    /** The key the walk stops before, if any. */
    std::optional<ustore_key_t> end_key;

    /** The verbs and the match of the home the walk computes at. */
    cursor_engines<docs_verbs> engine;
};

/** What @c ustore_pages_cursor_t points to: a walk over the keys of one pages collection, resumed
 *  as the blobs walk is. */
struct ustore_pages_cursor_handle_t {
    cursor_t cursor;

    /** The first key in the walk's direction the next pull may hand out, or the end of the walk. */
    std::variant<ustore_key_t, exhausted_t> position;

    /** The key the walk stops before, if any. */
    std::optional<ustore_key_t> end_key;

    /** The verbs and the match of the home the walk computes at. */
    cursor_engines<pages_verbs> engine;
};

/** What @c ustore_graph_vertices_cursor_t points to: a walk over the vertices of one graph
 *  collection, resumed as the blobs walk is. */
struct ustore_graph_vertices_cursor_handle_t {
    cursor_t cursor;

    /** The first key in the walk's direction the next pull may hand out, or the end of the walk. */
    std::variant<ustore_key_t, exhausted_t> position;

    /** The key the walk stops before, if any. */
    std::optional<ustore_key_t> end_key;

    /** The verbs and the match of the home the walk computes at. */
    cursor_engines<graph_verbs> engine;
};

/** What @c ustore_strings_cursor_t points to: a walk over the strings of one prefix, resumed at the
 *  string after the last one it handed out. */
struct ustore_strings_cursor_handle_t {

    /** What the walk holds and reads. */
    cursor_t cursor;

    /** The prefix every string the walk hands out starts with. */
    sz::string_t prefix;

    /** The string the next pull starts at, or the end of the walk. */
    std::variant<sz::string_t, exhausted_t> position;

    /** Whether the next pull hands out the string it starts at, which a descending walk skips. */
    start_bound_t position_bound = start_bound_t::inclusive_k;

    /** The string the walk stops before, where an empty one bounds nothing. */
    sz::string_t end_string;

    /** The verbs and the match of the home the walk computes at. */
    cursor_engines<strings_verbs> engine;
};

/** What @c ustore_changes_cursor_t points to: the claim of the pull filling it, the database and
 *  collection it reads, and its engine's state, which holds the pinned stamp and the position of
 *  the next pull. */
struct ustore_changes_cursor_handle_t {
    cursor_t cursor;
    void* state = nullptr;
};
