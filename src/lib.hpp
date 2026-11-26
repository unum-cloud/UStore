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
 *  database opened read-only, then claims its executor, runs the call on the calling thread or
 *  delegates it to the executor's pool thread 0, and finishes its outcome when the engine returns.
 *
 *  A call keeps whatever it needs only while it runs in a scratch, so its arena holds only its
 *  outputs. Each pool thread of an executor keeps one scratch for the slices it serves, and the
 *  call holding the executor one more, all rewound as a call starts and kept at a high-water mark
 *  until the executor is freed. A call without an executor makes a scratch of its own on the
 *  calling thread, starting from one page of its stack, and frees whatever it grew into once final.
 *  The router gathers there the views every accessor of a ragged input writes, on the thread that
 *  runs the call, before the engine sees it, so every engine reads tapes and accessors alike.
 *
 *  An executor serves one call at a time, held from its claim to its end, and a second call from
 *  any thread finishes contended, so nothing queues. A blocking call, and every call on a
 *  caller-inclusive pool, runs on the calling thread, and a VRAM executor's one host thread is its
 *  caller, which stages, feeds the device and waits on it. A deferred call on a caller-exclusive
 *  pool is copied into the executor and dispatched to pool thread 0, which serves it in the
 *  caller's place while the other threads return at once, so polling alone finishes it. Inside the
 *  call every fork of the pool forks the other threads, so a verb splits its batch the same way in
 *  both modes. A call ends in one order: its own claims and handle counts, the executor claim, then
 *  the message and the status with release ordering, so a caller seeing it final may submit again
 *  or free at once. The next call, a drain or a free first joins that dispatch.
 *
 *  Every executor call raises the count of its database, of the transaction, snapshot or cursor it
 *  names, and of its arena at its claim, and lowers them at its end, so freeing any of those
 *  handles waits for zero.
 *
 *  Cursors live here, with no engine slot of their own: a pull lists the next page through the engine's scan and reads
 *  its values through the engine's read in one call. A cursor holds its database, and the transaction or snapshot it
 *  reads through, as a transaction holds its database, so each closes with the last of its holds.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`, `std::uintptr_t`

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

#include "ustore/utilities/blocking_files.hpp" // `absolute_path_of`, `path_max_bytes_k`
#include "ustore/utilities/ram_buffers.hpp"    // `arena_rewind`, `arena_page_bytes_k`
#include "ustore/utilities/ram_device.hpp"     // `claim_t`

#include "engine.hpp"             // `engine_t`, `ustore_*_handle_t`, `view_t`, `host_verbs`, `floor_atomic_ref`
#include "match.hpp"              // `compiled_match`, `compile_match`, `scan_matched`, `exhausted_t`
#include "utilities/batches.hpp"  // `strings_arg_t`
#include "utilities/progress.hpp" // `progress_ticks_t`
#include "vram/classes.hpp"       // `gpu_class_t`, `gpu_class_mask_t`, `loaded_gpu_classes`, `kernel_tables`
#include "vram/matching.hpp"      // `compile_match`, `scan_matched_on_device`
#include "vram/staging.hpp"       // `stage_inputs`

namespace unum::ustore {

/** What every cursor holds: the claim of the pull filling it, its deferred pulls, and what it walks. */
struct cursor_t {

    /** Whether a pull fills the cursor, so a second pull naming it finds it contended. */
    claim_t claim = claim_t::free_k;

    /** Executor pulls naming the cursor that have not ended, through the floor reference. */
    std::uint32_t named_calls = 0;

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

/** The verbs a walk lists and reads through at one residency, and the match it compiled at init. */
template <template <vendored_residency_t> typename verbs_type_, vendored_residency_t residency_>
struct cursor_engine {
    verbs_type_<residency_> const* verbs = nullptr;
    compiled_match<residency_> match;
};

/** The engine of a walk at whichever residency its init's executor serves. */
template <template <vendored_residency_t> typename verbs_type_>
using cursor_engines = std::variant<cursor_engine<verbs_type_, vendored_residency_t::host_k>,
                                    cursor_engine<verbs_type_, vendored_residency_t::vram_nvidia_k>,
                                    cursor_engine<verbs_type_, vendored_residency_t::vram_amd_k>,
                                    cursor_engine<verbs_type_, vendored_residency_t::vram_apple_k>>;

#pragma region Names

/** One enumerator of a closed set and its canonical spelling. */
template <typename enum_type_>
struct enumerator_spelling {
    enum_type_ value;
    std::string_view name;
};

/** The spelling of @p value in @p names, or "unrecognized" for a value no enumerator holds. */
template <typename enum_type_, std::size_t count_>
char const* name_in(enumerator_spelling<enum_type_> const (&names)[count_], enum_type_ const& value) noexcept {
    // A C caller may pass any integer, which C++ may not load as the enum, so its bits are read in place instead
    using bits_t = std::underlying_type_t<enum_type_>;
    for (enumerator_spelling<enum_type_> const& spelling : names)
        if (std::bit_cast<bits_t>(spelling.value) == std::bit_cast<bits_t>(value)) return spelling.name.data();
    return "unrecognized";
}

/** Writes to @p value the enumerator of @p names spelled @p name, or refuses a name none
 *  holds. */
template <typename enum_type_, std::size_t count_>
ustore_status_t value_in(enumerator_spelling<enum_type_> const (&names)[count_], ustore_bytes_view_t name,
                         enum_type_* value) noexcept {
    if (!value) return ustore_status_invalid_argument_k;
    std::string_view const text = util::chars_of(name);
    for (enumerator_spelling<enum_type_> const& spelling : names)
        if (spelling.name == text) return *value = spelling.value, ustore_status_success_k;
    return ustore_status_invalid_argument_k;
}

#pragma endregion Names

#pragma region GPU Classes

/** Whether the build compiled the bundle of @p gpu_class, as CMake decided and told every unit
 *  through @c USTORE_TARGET_<CLASS>. */
constexpr bool gpu_class_compiled(gpu_class_t gpu_class) noexcept {
    switch (gpu_class) {
#if USTORE_TARGET_NVIDIA_AMPERE
    case gpu_class_t::nvidia_ampere_k: return true;
#endif
#if USTORE_TARGET_NVIDIA_ADA
    case gpu_class_t::nvidia_ada_k: return true;
#endif
#if USTORE_TARGET_NVIDIA_HOPPER
    case gpu_class_t::nvidia_hopper_k: return true;
#endif
#if USTORE_TARGET_NVIDIA_BLACKWELL
    case gpu_class_t::nvidia_blackwell_k: return true;
#endif
#if USTORE_TARGET_NVIDIA_BLACKWELLRTX
    case gpu_class_t::nvidia_blackwellrtx_k: return true;
#endif
#if USTORE_TARGET_AMD_CDNA3
    case gpu_class_t::amd_cdna3_k: return true;
#endif
#if USTORE_TARGET_AMD_CDNA4
    case gpu_class_t::amd_cdna4_k: return true;
#endif
#if USTORE_TARGET_AMD_CDNA5
    case gpu_class_t::amd_cdna5_k: return true;
#endif
#if USTORE_TARGET_APPLE_METAL7
    case gpu_class_t::apple_metal7_k: return true;
#endif
#if USTORE_TARGET_APPLE_METAL9
    case gpu_class_t::apple_metal9_k: return true;
#endif
#if USTORE_TARGET_APPLE_METAL10
    case gpu_class_t::apple_metal10_k: return true;
#endif
    default: return false;
    }
}

/** Rungs of a ladder this build compiled; a monoversioned build compiles at most one. */
template <gpu_class_t... classes_>
consteval std::size_t compiled_rungs(gpu_ladder<classes_...>) noexcept {
    return (std::size_t {gpu_class_compiled(classes_)} + ... + 0);
}

#if USTORE_TARGET_MONOVERSIONED
static_assert(compiled_rungs(nvidia_ladder_t {}) <= 1);
static_assert(compiled_rungs(amd_ladder_t {}) <= 1);
static_assert(compiled_rungs(apple_ladder_t {}) <= 1);
#endif

/** Vendors one process drives, each listing at most @c gpu_devices_capacity_k devices. */
inline constexpr std::size_t gpu_vendors_count_k = 3;

#pragma region Shared VRAM

/** The GPU side of the process, built at its first VRAM call: every vendor's driver, every device
 *  they report, NVIDIA first, then AMD, then Apple, a device's position here being its ordinal in
 *  the C interface, the classes whose bundles loaded on each, and every vendor's kernels, loaded
 *  once per device and shared by every executor there. A device no class runs on is listed too and
 *  refuses all work. */
struct gpu_runtime_t {
    util::gpu_drivers_t drivers;
    util::gpu_device_t devices[3 * util::gpu_devices_capacity_k];
    gpu_class_mask_t loaded[3 * util::gpu_devices_capacity_k] = {};
    kernel_tables<util::cuda_module_t> cuda_kernels;
    kernel_tables<util::hip_module_t> hip_kernels;
    kernel_tables<util::metal_module_t> metal_kernels;
    std::size_t devices_count = 0;

    gpu_runtime_t() noexcept;
};

/** The one runtime every VRAM call passes its drivers down from. */
inline gpu_runtime_t& gpu_runtime() noexcept {
    static gpu_runtime_t runtime;
    return runtime;
}

/** Every vendor's driver, loaded at the first VRAM call. */
inline util::gpu_drivers_t& gpu_drivers() noexcept { return gpu_runtime().drivers; }

/** Every GPU the drivers report, as the ABI numbers them. */
inline std::span<util::gpu_device_t const> gpu_devices() noexcept {
    gpu_runtime_t const& runtime = gpu_runtime();
    return {runtime.devices, runtime.devices_count};
}

/** The classes whose bundles load on each device @ref gpu_devices lists, asked once at listing. */
inline std::span<gpu_class_mask_t const> gpu_classes_loaded() noexcept {
    gpu_runtime_t const& runtime = gpu_runtime();
    return {runtime.loaded, runtime.devices_count};
}

#pragma endregion Shared VRAM

inline gpu_runtime_t::gpu_runtime_t() noexcept {
    std::size_t const reported = util::list_gpu_devices(drivers, devices);
    for (std::size_t position = 0; position != reported; ++position) {
        util::gpu_device_t const device = devices[position];
        if (device.vendor_ordinal >= util::gpu_devices_capacity_k) continue;
        loaded[devices_count] = loaded_gpu_classes(drivers, device);
        devices[devices_count++] = device;
    }
}

/**
 *  @brief Spawns into @p into one executor of @p gpu_class over every entry of @p devices, on
 *      @p streams when given, as @c util::spawn_gpu_executor does, each stream's scratch capped at
 *      @p scratch_limit bytes, zero for no ceiling.
 *
 *  Each stream launches from the table of its device and class, which the first spawn there loads
 *  while holding the device's claim, so no other spawn loads onto it; a device the class's bundle
 *  compiles no code for keeps moving memory and refuses launches alone.
 */
template <typename variant_type_>
util::status_t spawn_vram_executor(gpu_runtime_t& runtime, std::span<util::gpu_device_t const> devices,
                                   gpu_class_t gpu_class, std::size_t scratch_limit, std::span<void* const> streams,
                                   variant_type_& into) noexcept {
    util::gpu_drivers_t& drivers = runtime.drivers;
    auto const load = [&](auto& tables, auto const& api, auto place, int ordinal, std::span<char const* const> names,
                          auto... options) noexcept {
        class_verbs_t const& verbs = host_verbs();
        claim_t& opening = tables.opening(ordinal);
        while (!verbs.take(&opening)) verbs.wait_while_claimed(&opening);
        [[maybe_unused]] util::status_t const loaded = tables.load(api, place, ordinal, gpu_class, names, options...);
        verbs.release(&opening);
        return tables.table_of(ordinal, gpu_class);
    };
    auto kernels_of = [&]<typename module_type_>(std::type_identity<module_type_>, int ordinal) noexcept {
        if constexpr (std::is_same_v<module_type_, util::cuda_module_t>)
            return load(runtime.cuda_kernels, drivers.cuda, ordinal, ordinal, util::vram_kernel_names_k);
        else if constexpr (std::is_same_v<module_type_, util::hip_module_t>)
            return load(runtime.hip_kernels, drivers.hip, ordinal, ordinal, util::vram_kernel_names_k);
        else {
            // The kernels need MSL 3.2's device-scope atomic fences and `gpuAddress` casts
            util::metal_context_t const* const context = drivers.metal_contexts.context(ordinal);
            return load(runtime.metal_kernels, drivers.metal, context ? context->device : nullptr, ordinal,
                        util::vram_apple_kernel_names_k, util::metal_language_t::msl_3_2_k);
        }
    };
    auto [spawned, spawn_status] = util::spawn_gpu_executor(drivers, devices, streams, kernels_of, scratch_limit);
    if (spawn_status.failed()) return spawn_status;
    std::visit(
        [&]<typename executor_type_>(executor_type_& executor) noexcept {
            if constexpr (util::a_gpu_executor<executor_type_>)
                into.template emplace<executor_type_>(std::move(executor));
        },
        spawned);
    return {};
}

/** Frees every block of @p arena wherever it lives, a device's runs through the process's drivers as the executor that
 *  made the arena may be gone, and the arena itself, where null is safe. */
inline void destroy_arena(ustore_arena_t arena) noexcept {
    if (!arena) return;
    if (util::cuda_blocks_t* const memory = std::get_if<util::cuda_blocks_t>(&arena->memory)) {
        util::gpu_drivers_t const& drivers = gpu_drivers();
        util::free_cuda_blocks(drivers.cuda, memory->device, memory->blocks);
    }
    else if (util::hip_blocks_t* const memory = std::get_if<util::hip_blocks_t>(&arena->memory))
        util::free_hip_blocks(gpu_drivers().hip, memory->device, memory->blocks);
    else if (util::metal_blocks_t* const memory = std::get_if<util::metal_blocks_t>(&arena->memory)) {
        if (util::metal_context_t* const context = gpu_drivers().metal_contexts.context(memory->device))
            util::free_metal_blocks(*context, memory->blocks);
    }
    else util::arena_free_blocks(*arena);
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

/** The transaction a call holds, or null. */
inline ustore_transaction_t held(ustore_transaction_t transaction) noexcept { return transaction; }

/** The transaction a call reuses, or null for one it opens afresh. */
inline ustore_transaction_t held(ustore_transaction_t const* transaction) noexcept {
    return transaction ? *transaction : nullptr;
}

/** The snapshot a call reads, or null, which an output for a new snapshot never matches. */
inline ustore_snapshot_t viewed(ustore_snapshot_t snapshot) noexcept { return snapshot; }

/** Whether a call reads through or reuses a transaction. */
template <typename arguments_type_>
concept a_call_with_transaction = requires(arguments_type_ const& arguments) { held(arguments.transaction); };

/** Whether a call reads through a snapshot, or answers a new one. */
template <typename arguments_type_>
concept a_call_reading_snapshot = requires(arguments_type_ const& arguments) { viewed(arguments.snapshot); };

/** Decodes what @p arguments read through into @p view, answering false for a view neither view
 *  constructor made. */
template <typename arguments_type_>
bool decode_call(arguments_type_ const& arguments, view_t& view) noexcept {
    if constexpr (a_call_reading_view<arguments_type_>)
        if (!decode_view(arguments.view, view)) return false;
    if constexpr (a_call_with_transaction<arguments_type_>) view.transaction = held(arguments.transaction);
    if constexpr (a_call_naming_watch<arguments_type_>) view.watch = arguments.watch;
    if constexpr (a_call_reading_snapshot<arguments_type_>) view.snapshot = viewed(arguments.snapshot);
    return true;
}

/** A pull reads through the view its cursor opened with. */
template <typename arguments_type_>
    requires a_cursor_walker<arguments_type_>
bool decode_call(arguments_type_ const& arguments, view_t& view) noexcept {
    return decode_view(arguments.cursor->walk.view, view);
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

/** Whether a call rewrites the schema or the trees an open ingest holds until its finish, which
 *  this very caller may own, so the call refuses rather than waits for it. */
template <typename arguments_type_>
constexpr bool outlasted_by_ingests =
    one_of<arguments_type_, ustore_collection_create_t, ustore_collection_drop_t, ustore_database_clear_t,
           ustore_database_compact_t, ustore_database_flush_t>;

/** Refuses with @c contended_k while an ingest is open on @p database, changing nothing. */
inline util::status_t refuse_open_ingests(ustore_database_handle_t& database) noexcept {
    if (floor_atomic_ref<std::uint32_t> {database.open_ingests}.load(std::memory_order_acquire))
        return {util::status_code_t::contended_k, "An ingest is open on the database"};
    return {};
}

/** Whether a call changes its database, which one opened read-only refuses: every call naming a
 *  transaction, as reads name views instead, and the calls on whole collections. */
template <typename arguments_type_>
constexpr bool changes_database = requires(arguments_type_ const& arguments) { arguments.transaction; } ||
                                  one_of<arguments_type_, ustore_collection_create_t, ustore_collection_drop_t,
                                         ustore_database_clear_t, ustore_database_compact_t, ustore_database_flush_t>;

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
    clear(arguments.batches), clear(arguments.batches_count);
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

/** Whether a call has outputs to leave null or zero before it runs and after it fails. */
template <typename arguments_type_>
concept a_call_clearing_outputs = requires(arguments_type_ const& arguments) { clear_outputs(arguments); };

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

/** Whether a call takes ragged inputs, which the engines read gathered on the host. */
template <typename arguments_type_>
concept a_call_with_ragged_inputs = requires(arguments_type_ const& arguments) { ragged_inputs(arguments); };

/** Strings an accessor writes per block, so the indices it reads fit on the stack of the thread running the call. */
constexpr std::size_t accessor_block_k = 64;

/** Refuses a tape of @p count strings whose offsets step back or outrun its bytes. */
template <typename tape_type_>
util::status_t tape_holds(tape_type_ const& tape, std::size_t count) noexcept {
    for (std::size_t index = 0; index != count; ++index)
        if (tape.offsets[index + 1] < tape.offsets[index])
            return {util::status_code_t::invalid_argument_k, "A tape of strings needs ascending offsets"};
    if (!count || tape.data || tape.offsets[count] == tape.offsets[0]) return {};
    return {util::status_code_t::invalid_argument_k, "A tape of strings needs its bytes"};
}

/** Reads @p input for its engine into @p strings, refusing a layout no constant names and gathering
 *  into @p scratch the views an accessor writes, so the accessor runs on the thread running the
 *  call and never after it is final. */
inline util::status_t gather_strings(ragged_input_t const& input, ustore_arena_t scratch,
                                     strings_arg_t& strings) noexcept {
    ustore_strings_view_t const& view = input.view;
    strings.view = view;
    // A C caller may pass any integer, which C++ may not load as the enum, so its bits are read instead
    switch (std::bit_cast<std::underlying_type_t<ustore_strings_layout_t>>(view.layout)) {
    case ustore_strings_u32_tape_k:
        if (!view.data.u32_tape.offsets) return {};
        return tape_holds(view.data.u32_tape, input.count);
    case ustore_strings_u64_tape_k:
        if (input.count && !view.data.u64_tape.offsets)
            return {util::status_code_t::invalid_argument_k, "A tape of strings needs its offsets"};
        return tape_holds(view.data.u64_tape, input.count);
    case ustore_strings_accessor_k: break;
    default: return {util::status_code_t::invalid_argument_k, "The strings come in an unknown layout"};
    }
    if (!input.count) return {};
    if (!view.data.accessor.get)
        return {util::status_code_t::invalid_argument_k, "An accessor of strings needs its get"};

    auto [views, status] = util::arena_allocate<ustore_bytes_view_t>(scratch, input.count);
    if (status.failed()) return status;
    std::array<ustore_size_t, accessor_block_k> indices;
    for (std::size_t first = 0; first < input.count; first += indices.size()) {
        std::size_t const block = fu::min_of(indices.size(), input.count - first);
        std::iota(indices.begin(), indices.begin() + block, static_cast<ustore_size_t>(first));
        view.data.accessor.get(view.data.accessor.handle, indices.data(), block, views.data() + first);
    }
    strings.gathered = views.data();
    return {};
}

#pragma endregion Ragged Inputs

#pragma region Strings Windows

/** The strings one task of a strings call names: those starting with @c prefix, from @c start,
 *  inclusive, towards @c end, exclusive, in @c direction, where an empty start begins at the first
 *  string in the direction and an empty end bounds nothing. */
struct strings_window_t {
    std::string_view prefix;
    std::string_view start;
    std::string_view end;
    ustore_scan_direction_t direction = ustore_scan_ascending_k;

    /** The least string of the window, or of the prefix where the window names none. */
    [[nodiscard]] std::string_view lower() const noexcept {
        std::string_view const bound = direction == ustore_scan_descending_k ? end : start;
        return fu::max_of(bound, prefix);
    }

    /** The string the window stops at, exclusive for an ascending window and inclusive for a
     *  descending one, or empty for none. */
    [[nodiscard]] std::string_view upper() const noexcept {
        return direction == ustore_scan_descending_k ? start : end;
    }

    /** Whether @p text, prefixed and not below @c lower, lies past the window's upper end. */
    [[nodiscard]] bool past(std::string_view text) const noexcept {
        std::string_view const bound = upper();
        if (bound.empty()) return false;
        return direction == ustore_scan_descending_k ? bound < text : !(text < bound);
    }

    /** Whether @p text is the exclusive end a descending window starts above. */
    [[nodiscard]] bool skips(std::string_view text) const noexcept {
        return direction == ustore_scan_descending_k && !end.empty() && text == end;
    }
};

#pragma endregion Strings Windows

#pragma region Cursors

/**
 *  @brief Pulls the next page of the walk @p arguments name through @p engine, as one call over the arena they give.
 *
 *  The engine's scan lists every page into the arena, which the match narrows, until the survivors fill the page or
 *  the collection ends; then the arena is rewound, the read writes the values of the survivors there when asked, and
 *  the keys follow them, so the arena holds only the outputs. The cursor advances only once the progress callback let
 *  the pull finish.
 */
util::status_t pull(engine_t const& engine, ustore_blobs_cursor_next_t const& arguments,
                    ustore_blobs_cursor_handle_t& cursor, view_t const& view) noexcept;

/** Pulls the next keys of a walk over documents as batches of rows with the columns of the cursor,
 *  one per task of the executor, which a host engine splits the pull into. */
util::status_t pull(engine_t const& engine, ustore_docs_cursor_next_t const& arguments,
                    ustore_docs_cursor_handle_t& cursor, view_t const& view) noexcept;

/** Pulls the next page of a walk over pages, as the blobs pull does, rendering whole pages when asked. */
util::status_t pull(engine_t const& engine, ustore_pages_cursor_next_t const& arguments,
                    ustore_pages_cursor_handle_t& cursor, view_t const& view) noexcept;

/** Pulls the next page of a walk over vertices, as the blobs pull does. */
util::status_t pull(engine_t const& engine, ustore_graph_vertices_cursor_next_t const& arguments,
                    ustore_graph_vertices_cursor_handle_t& cursor, view_t const& view) noexcept;

/** Pulls the next page of the walk @p arguments name through @p engine, as the blobs pull does,
 *  resuming past the last string at that string followed by a zero byte, the smallest string after
 *  it. */
util::status_t pull(engine_t const& engine, ustore_strings_cursor_next_t const& arguments,
                    ustore_strings_cursor_handle_t& cursor, view_t const& view) noexcept;

/** The bodies of the pulls above, narrowing through @p match, the walk's own or the face a device
 *  pull shows it, each defined beside its pull. */
template <typename match_type_>
util::status_t pull_through(engine_t const& engine, ustore_blobs_cursor_next_t const& arguments,
                            ustore_blobs_cursor_handle_t& cursor, match_type_ const& match,
                            view_t const& view) noexcept;
template <typename match_type_>
util::status_t pull_through(engine_t const& engine, ustore_docs_cursor_next_t const& arguments,
                            ustore_docs_cursor_handle_t& cursor, match_type_ const& match, view_t const& view) noexcept;
template <typename match_type_>
util::status_t pull_through(engine_t const& engine, ustore_pages_cursor_next_t const& arguments,
                            ustore_pages_cursor_handle_t& cursor, match_type_ const& match,
                            view_t const& view) noexcept;
template <typename match_type_>
util::status_t pull_through(engine_t const& engine, ustore_graph_vertices_cursor_next_t const& arguments,
                            ustore_graph_vertices_cursor_handle_t& cursor, match_type_ const& match,
                            view_t const& view) noexcept;
template <typename match_type_>
util::status_t pull_through(engine_t const& engine, ustore_strings_cursor_next_t const& arguments,
                            ustore_strings_cursor_handle_t& cursor, match_type_ const& match,
                            view_t const& view) noexcept;

/** Opens the change feed @p arguments ask for through @p engine, which pins its stamp, holding the
 *  database open until the feed is freed. */
util::status_t open_changes_cursor(engine_t const& engine, ustore_changes_cursor_init_t const& arguments,
                                   view_t const& view) noexcept;

/** Pulls the next changes of the feed @p arguments name through @p engine. */
util::status_t pull(engine_t const& engine, ustore_changes_cursor_next_t const& arguments,
                    ustore_changes_cursor_handle_t& cursor, view_t const& view) noexcept;

/** Refuses a feed pulled on a device, as change feeds read host runs alone. */
template <util::a_gpu_stream executor_type_>
util::status_t pull(engine_t const&, ustore_changes_cursor_next_t const&, ustore_changes_cursor_handle_t&,
                    view_t const&, executor_type_&) noexcept {
    return {util::status_code_t::unsupported_k, "Change feeds run on host executors"};
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
 *  The host lists every page into a device's bounce, dealing pages across the executor's devices,
 *  each device matches its pages' values, and every output moves into the call's device arena on
 *  the first device once. The cursor's engine compiled at its init into the cursor's own arena,
 *  which outlives every call, and each pull rebases it onto a copy of its tables in every device's
 *  managed runs the call rewinds. A cursor opened on the host walks there alone. The pull body of
 *  every walk, @c pull_through, is defined where its modality's pull is.
 */
template <typename arguments_type_, typename cursor_type_, util::a_gpu_stream executor_type_>
util::status_t pull(engine_t const& engine, arguments_type_ const& arguments, cursor_type_& cursor, view_t const& view,
                    executor_type_& executor) noexcept {
    constexpr vendored_residency_t residency_k = vendored_residency_of<executor_type_>;
    // The walks' engines list the residencies in their own order, the host's first
    constexpr std::size_t residency_index_k = static_cast<std::size_t>(residency_k);
    auto const* const walker = std::get_if<residency_index_k>(&cursor.engine);
    if (!walker) return {util::status_code_t::device_k, "The cursor walks on another kind of executor"};
    util::device_blocks_t* const blocks = util::device_blocks_of(arguments.arena, executor);
    if (!blocks) return {util::status_code_t::device_k, "The arena lives where another executor computes"};
    compiled_match<residency_k> match = walker->match;
    if (match.reads_values() && !executor.kernels)
        return {util::status_code_t::unsupported_k, "The device loaded no kernel bundle"};
    if (util::status_t const status = rebase_on_device(match, executor); status.failed()) return status;
    // The pull drives every stream of the executor, whose claim `visit_residency` holds
    residency_executor<residency_k>& devices = std::get<residency_executor<residency_k>>(view.executor->device);
    auto [matches, matches_status] = match_on_every_device(match, devices, view.scratch);
    if (matches_status.failed()) return matches_status;
    device_match<residency_k> const matching {matches, devices};
    arguments_type_ hosted = arguments;
    hosted.arena = executor.memory.bounce.arena();
    if (util::status_t const status = pull_through(engine, hosted, cursor, matching, view); status.failed())
        return status;
    if (util::status_t const bound = executor.bind(); bound.failed()) return bound;
    return upload_output_columns(hosted, *blocks, executor);
}

/** The keys @p cursor walks, pulled into @p survivors through @p engine and @p match, the walk's
 *  own or its device face, as one page of @p wanted, advancing @p resume past them, for every
 *  pull over keys. */
template <typename scan_type_, typename cursor_type_, typename match_type_>
util::status_t list_cursor_keys(engine_t const& engine, ustore_database_t database, cursor_type_& cursor,
                                match_type_ const& match, std::size_t wanted, ustore_arena_t arena, view_t const& view,
                                keyed_survivors_t& survivors,
                                std::variant<ustore_key_t, exhausted_t>& resume) noexcept {
    resume = cursor.position;
    ustore_key_t const* const start = std::get_if<ustore_key_t>(&cursor.position);
    if (!start || !wanted) return {};
    return list_matched_keys<scan_type_>(engine.host, database, cursor.walk.collection, *start,
                                         cursor.end_key ? std::addressof(*cursor.end_key) : nullptr,
                                         cursor.walk.direction, wanted, match, arena, view, survivors, resume);
}

#pragma endregion Cursors

/** Hands @p action @p view with every verb of the call reading at one state the engine of
 *  @p database holds for it: a reading of @p collection, the one the call reads where it is not
 *  null, or a pinned stamp; or @p view itself inside a transaction or a snapshot, or for an engine
 *  pinning no stamps. */
template <typename action_type_>
util::status_t with_call_stamp(engine_t const& engine, ustore_database_t database,
                               ustore_collection_id_t const* collection, view_t const& view,
                               action_type_&& action) noexcept {
    if (view.transaction || view.snapshot || view.stamp || view.reading || !engine.database->pin_call)
        return action(view);
    call_pin_t pin;
    engine.database->pin_call(database, collection, view, pin);
    view_t pinned = view;
    pinned.stamp = pin.stamp;
    pinned.reading = pin.reading;
    return action(pinned);
}

/** The one collection every task of @p arguments names, or null for a call naming several. */
template <typename arguments_type_>
ustore_collection_id_t const* one_collection(arguments_type_ const& arguments) noexcept {
    util::strided_iterator<ustore_collection_id_t const> const collections = util::strided(arguments.collections);
    if (!arguments.count) return nullptr;
    for (std::size_t task = 1; task != arguments.count; ++task)
        if (!(collections[static_cast<std::ptrdiff_t>(task)] == collections[0])) return nullptr;
    return &collections[0];
}

/** Finishes a call: the message, the engine's note or else the status's own name, then the status
 *  with release ordering, which its waiters watch by address. */
inline void finish(ustore_outcome_t& outcome, util::status_t status) noexcept {
    outcome.message = status.failed()
                          ? (status.note ? status.note : ustore_status_name(static_cast<ustore_status_t>(status.code)))
                          : nullptr;
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
    if constexpr (a_call_filling_arena<arguments_type_>) return arguments.arena;
    else return nullptr;
}

/** The transaction @p arguments hold through their view or their transaction, or null. */
template <typename arguments_type_>
ustore_transaction_t transaction_of(arguments_type_ const& arguments) noexcept {
    view_t view;
    return decode_call(arguments, view) ? view.transaction : nullptr;
}

/** The named-call counts of the database, of the cursor @p arguments pull, the live plan they
 *  continue or the transaction or snapshot they read through, and of their arena, each null when
 *  absent; a cursor's own holds keep what it reads through alive. */
template <typename arguments_type_>
std::array<std::uint32_t*, 3> named_calls_of(arguments_type_ const& arguments) noexcept {
    ustore_arena_t const arena = arena_of(arguments);
    ustore_database_t const database = database_of(arguments);
    std::uint32_t* viewed = nullptr;
    if constexpr (a_cursor_walker<arguments_type_>) viewed = &arguments.cursor->walk.named_calls;
    else if constexpr (a_call_feeding_ingest<arguments_type_>) viewed = &arguments.ingest->named_calls;
    else if constexpr (std::is_same_v<arguments_type_, ustore_database_compact_t>)
        viewed = arguments.plan && *arguments.plan ? &(*arguments.plan)->named_calls : nullptr;
    else {
        view_t view;
        decode_call(arguments, view);
        if (view.transaction) viewed = &view.transaction->named_calls;
        else if (view.snapshot) viewed = &view.snapshot->named_calls;
    }
    return {database ? &database->named_calls : nullptr, viewed, arena ? &arena->named_calls : nullptr};
}

/** An init names no handle yet, only an output for the database it opens, so it counts nowhere. */
template <typename arguments_type_>
    requires one_of<arguments_type_, ustore_database_init_ram_t, ustore_database_open_ram_t,
                    ustore_database_open_rocksdb_t, ustore_database_view_rocksdb_t, ustore_database_open_leveldb_t,
                    ustore_database_open_nvme_t, ustore_database_view_nvme_t>
std::array<std::uint32_t*, 3> named_calls_of(arguments_type_ const&) noexcept {
    return {};
}

/** Counts one more call on every handle @p arguments name, answering the counts to lower at its
 *  end, before the call runs, as a transaction init rewrites the handle its arguments point to. */
template <typename arguments_type_>
std::array<std::uint32_t*, 3> raise_named_calls(arguments_type_ const& arguments) noexcept {
    std::array<std::uint32_t*, 3> const counts = named_calls_of(arguments);
    for (std::uint32_t* const count : counts)
        if (count) floor_atomic_ref<std::uint32_t> {*count}.fetch_add(1, std::memory_order_relaxed);
    return counts;
}

/** Counts one call off each of @p counts that is not null, so a free waiting for zero goes on. */
inline void lower_named_calls(std::array<std::uint32_t*, 3> const& counts) noexcept {
    for (std::uint32_t* const count : counts)
        if (count) floor_atomic_ref<std::uint32_t> {*count}.fetch_sub(1, std::memory_order_release);
}

/** The claim of the cursor @p arguments pull or of the ingest they feed, or null for neither. */
template <typename arguments_type_>
claim_t* handle_claim_of(arguments_type_ const& arguments) noexcept {
    if constexpr (a_cursor_walker<arguments_type_>) return &arguments.cursor->walk.claim;
    else if constexpr (a_call_feeding_ingest<arguments_type_>) return &arguments.ingest->claim;
    else return nullptr;
}

/** The residency of the calls on @p executor; a null one runs them on the caller in host pages. */
inline vendored_residency_t residency_of(ustore_executor_handle_t const* executor) noexcept {
    if (!executor) return vendored_residency_t::host_k;
    if (std::holds_alternative<util::cuda_executor_t>(executor->device)) return vendored_residency_t::vram_nvidia_k;
    if (std::holds_alternative<util::hip_executor_t>(executor->device)) return vendored_residency_t::vram_amd_k;
    if (std::holds_alternative<util::metal_executor_t>(executor->device)) return vendored_residency_t::vram_apple_k;
    return vendored_residency_t::host_k;
}

/** Whether @p arena lives where calls on @p executor compute: host pages for host executors, the
 *  device of a VRAM executor for its calls. */
inline bool arena_serves(ustore_executor_handle_t const* executor, ustore_arena_t arena) noexcept {
    if (!executor) return std::holds_alternative<util::host_blocks_t>(arena->memory);
    return std::visit(
        [&]<typename kind_type_>(kind_type_ const& kind) noexcept {
            if constexpr (util::a_gpu_executor<kind_type_>)
                return util::device_blocks_of(arena, kind.stream(0)) != nullptr;
            else return std::holds_alternative<util::host_blocks_t>(arena->memory);
        },
        executor->device);
}

/** Whether @p arguments_type_ pulls a cursor, whose claim a contended call names. */
template <typename arguments_type_>
concept a_call_pulling_cursor = requires(arguments_type_ const& arguments) { arguments.cursor; };

/** Whether @p arguments_type_ names its keys through one keys view. */
template <typename arguments_type_>
concept a_call_viewing_keys = requires(arguments_type_ const& arguments) { arguments.keys.data; };

/** Whether an input of @p arguments lies inside @p arena, which the call rewinds before it
 *  reads it: their keys or the bytes and offsets of their ragged inputs. */
template <typename arguments_type_>
bool reads_from_arena(arguments_type_ const& arguments, ustore_arena_t arena) noexcept {
    if constexpr (a_call_viewing_keys<arguments_type_>)
        if (util::arena_holds(arena, arguments.keys.data)) return true;
    if constexpr (a_call_with_ragged_inputs<arguments_type_>)
        for (ragged_input_t const& input : ragged_inputs(arguments)) {
            ustore_strings_view_t const& view = input.view;
            if (view.layout == ustore_strings_u32_tape_k && (util::arena_holds(arena, view.data.u32_tape.data) ||
                                                             util::arena_holds(arena, view.data.u32_tape.offsets)))
                return true;
            if (view.layout == ustore_strings_u64_tape_k && (util::arena_holds(arena, view.data.u64_tape.data) ||
                                                             util::arena_holds(arena, view.data.u64_tape.offsets)))
                return true;
        }
    return false;
}

/** Hands every one of @p claims that is not null back to the next call. */
inline void release(std::array<claim_t*, 3> const& claims) noexcept {
    for (claim_t* const claim : claims)
        if (claim) release(*claim);
}

/** Claims the arena, the cursor or ingest and the transaction of @p arguments, answering the claims
 *  it took, or holding none when one serves another call, and refusing a call that needs an arena,
 *  a cursor or an ingest and names none, whose arena lives where its executor does not compute, or
 *  whose inputs lie inside the arena it rewinds. */
template <typename arguments_type_>
util::expected<std::array<claim_t*, 3>> take_claims(arguments_type_ const& arguments) noexcept {
    using util::status_code_t, util::status_t;
    if constexpr (a_call_pulling_cursor<arguments_type_>)
        if (!arguments.cursor) return status_t {status_code_t::invalid_argument_k, "The call needs a cursor"};
    if constexpr (a_call_feeding_ingest<arguments_type_>)
        if (!arguments.ingest) return status_t {status_code_t::invalid_argument_k, "The ingest is missing"};
    std::array<claim_t*, 3> claims {};
    if constexpr (a_call_filling_arena<arguments_type_>) {
        ustore_arena_t const arena = arguments.arena;
        if (!arena) return status_t {status_code_t::invalid_argument_k, "The call needs an arena for its outputs"};
        if (!arena_serves(arguments.executor, arena))
            return status_t {status_code_t::device_k, "The arena lives where another executor computes"};
        if (!take(arena->claim)) return status_t {status_code_t::contended_k, "The arena serves another call"};
        claims[0] = &arena->claim;
        if (reads_from_arena(arguments, arena))
            return release(claims),
                   status_t {status_code_t::invalid_argument_k, "Input lies inside the arena the call rewinds"};
    }
    claim_t* const handle = handle_claim_of(arguments);
    if (handle && !take(*handle))
        return release(claims), status_t {status_code_t::contended_k, a_call_feeding_ingest<arguments_type_>
                                                                          ? "The ingest serves another call"
                                                                          : "The cursor serves another pull"};
    claims[1] = handle;
    ustore_transaction_t const transaction = transaction_of(arguments);
    if (transaction && !take(transaction->claim))
        return release(claims), status_t {status_code_t::contended_k, "The transaction serves another call"};
    claims[2] = transaction ? &transaction->claim : nullptr;
    return claims;
}

/** Finishes @p arguments with @p status without serving them, releasing the @p claims the router
 *  took. */
template <typename arguments_type_>
void refuse(arguments_type_& arguments, std::array<claim_t*, 3> const& claims, util::status_t status) noexcept {
    release(claims);
    finish(*arguments.outcome, status);
}

/** Waits until no call counted in @p calls is left, so their handle may be freed. */
inline void wait_named_calls(std::uint32_t& calls) noexcept {
    floor_atomic_ref<std::uint32_t const> const counted {calls};
    for (std::uint32_t seen = counted.load(std::memory_order_acquire); seen != 0;
         seen = counted.load(std::memory_order_acquire))
        host_verbs().await_change(&calls, seen);
}

/** Frees @p plan and its engine plan once no call naming it is in flight, so another compaction may
 *  open on its database; null is safe. */
inline void free_compact_plan(ustore_database_compact_plan_t plan) noexcept {
    if (!plan) return;
    wait_named_calls(plan->named_calls);
    ustore_database_t const database = plan->opened_on;
    if (plan->engine_plan) engine_of(database).database->compact_plan.free(plan->engine_plan);
    release(database->compaction);
    delete plan;
}

/** Finishes a plan's finish with @p status without serving it, freeing the plan all the same; it
 *  takes no claims, and must stay a better match than the template so the plan is never leaked. */
inline void refuse(ustore_database_compact_plan_finish_t& arguments, std::array<claim_t*, 3> const&,
                   util::status_t status) noexcept {
    free_compact_plan(arguments.plan);
    finish(*arguments.outcome, status);
}

#pragma endregion Claims

#pragma region Verb Slots

/**
 *  @brief Where the verb serving an argument structure sits: @c table names the modality's table
 *      at a residency and @c verb the verb in it, each a variable template over the residency, or
 *      @c database the verb of the control plane, which serves on host executors alone.
 *
 *  A @c verb whose residency lacks the slot fails its constraint, as every change does on a device.
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
struct verb_slot<ustore_database_flush_t> {
    static constexpr auto database = &database_verbs_t::flush;
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
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::blobs;
    template <vendored_residency_t residency_>
        requires requires { &blobs_verbs<residency_>::write; }
    static constexpr auto verb = &blobs_verbs<residency_>::write;
};
template <>
struct verb_slot<ustore_blobs_remove_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::blobs;
    template <vendored_residency_t residency_>
        requires requires { &blobs_verbs<residency_>::remove; }
    static constexpr auto verb = &blobs_verbs<residency_>::remove;
};
template <>
struct verb_slot<ustore_blobs_read_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::blobs;
    template <vendored_residency_t residency_>
        requires requires { &blobs_verbs<residency_>::read; }
    static constexpr auto verb = &blobs_verbs<residency_>::read;
};
template <>
struct verb_slot<ustore_blobs_scan_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::blobs;
    template <vendored_residency_t residency_>
        requires requires { &blobs_verbs<residency_>::scan; }
    static constexpr auto verb = &blobs_verbs<residency_>::scan;
};
template <>
struct verb_slot<ustore_blobs_sample_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::blobs;
    template <vendored_residency_t residency_>
        requires requires { &blobs_verbs<residency_>::sample; }
    static constexpr auto verb = &blobs_verbs<residency_>::sample;
};
template <>
struct verb_slot<ustore_blobs_measure_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::blobs;
    template <vendored_residency_t residency_>
        requires requires { &blobs_verbs<residency_>::measure; }
    static constexpr auto verb = &blobs_verbs<residency_>::measure;
};
template <>
struct verb_slot<ustore_strings_write_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::strings;
    template <vendored_residency_t residency_>
        requires requires { &strings_verbs<residency_>::write; }
    static constexpr auto verb = &strings_verbs<residency_>::write;
};
template <>
struct verb_slot<ustore_strings_remove_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::strings;
    template <vendored_residency_t residency_>
        requires requires { &strings_verbs<residency_>::remove; }
    static constexpr auto verb = &strings_verbs<residency_>::remove;
};
template <>
struct verb_slot<ustore_strings_read_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::strings;
    template <vendored_residency_t residency_>
        requires requires { &strings_verbs<residency_>::read; }
    static constexpr auto verb = &strings_verbs<residency_>::read;
};
template <>
struct verb_slot<ustore_strings_scan_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::strings;
    template <vendored_residency_t residency_>
        requires requires { &strings_verbs<residency_>::scan; }
    static constexpr auto verb = &strings_verbs<residency_>::scan;
};
template <>
struct verb_slot<ustore_docs_write_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::docs;
    template <vendored_residency_t residency_>
        requires requires { &docs_verbs<residency_>::write; }
    static constexpr auto verb = &docs_verbs<residency_>::write;
};
template <>
struct verb_slot<ustore_docs_read_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::docs;
    template <vendored_residency_t residency_>
        requires requires { &docs_verbs<residency_>::read; }
    static constexpr auto verb = &docs_verbs<residency_>::read;
};
template <>
struct verb_slot<ustore_docs_gist_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::docs;
    template <vendored_residency_t residency_>
        requires requires { &docs_verbs<residency_>::gist; }
    static constexpr auto verb = &docs_verbs<residency_>::gist;
};
template <>
struct verb_slot<ustore_docs_gather_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::docs;
    template <vendored_residency_t residency_>
        requires requires { &docs_verbs<residency_>::gather; }
    static constexpr auto verb = &docs_verbs<residency_>::gather;
};
template <>
struct verb_slot<ustore_docs_remove_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::docs;
    template <vendored_residency_t residency_>
        requires requires { &docs_verbs<residency_>::remove; }
    static constexpr auto verb = &docs_verbs<residency_>::remove;
};
template <>
struct verb_slot<ustore_docs_scan_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::docs;
    template <vendored_residency_t residency_>
        requires requires { &docs_verbs<residency_>::scan; }
    static constexpr auto verb = &docs_verbs<residency_>::scan;
};
template <>
struct verb_slot<ustore_docs_sample_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::docs;
    template <vendored_residency_t residency_>
        requires requires { &docs_verbs<residency_>::sample; }
    static constexpr auto verb = &docs_verbs<residency_>::sample;
};
template <>
struct verb_slot<ustore_graph_read_edges_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::graph;
    template <vendored_residency_t residency_>
        requires requires { &graph_verbs<residency_>::read_edges; }
    static constexpr auto verb = &graph_verbs<residency_>::read_edges;
};
template <>
struct verb_slot<ustore_graph_write_edges_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::graph;
    template <vendored_residency_t residency_>
        requires requires { &graph_verbs<residency_>::write_edges; }
    static constexpr auto verb = &graph_verbs<residency_>::write_edges;
};
template <>
struct verb_slot<ustore_graph_remove_edges_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::graph;
    template <vendored_residency_t residency_>
        requires requires { &graph_verbs<residency_>::remove_edges; }
    static constexpr auto verb = &graph_verbs<residency_>::remove_edges;
};
template <>
struct verb_slot<ustore_graph_write_vertices_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::graph;
    template <vendored_residency_t residency_>
        requires requires { &graph_verbs<residency_>::write_vertices; }
    static constexpr auto verb = &graph_verbs<residency_>::write_vertices;
};
template <>
struct verb_slot<ustore_graph_remove_vertices_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::graph;
    template <vendored_residency_t residency_>
        requires requires { &graph_verbs<residency_>::remove_vertices; }
    static constexpr auto verb = &graph_verbs<residency_>::remove_vertices;
};
template <>
struct verb_slot<ustore_graph_scan_vertices_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::graph;
    template <vendored_residency_t residency_>
        requires requires { &graph_verbs<residency_>::scan_vertices; }
    static constexpr auto verb = &graph_verbs<residency_>::scan_vertices;
};
template <>
struct verb_slot<ustore_pages_import_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::pages;
    template <vendored_residency_t residency_>
        requires requires { &pages_verbs<residency_>::import; }
    static constexpr auto verb = &pages_verbs<residency_>::import;
};
template <>
struct verb_slot<ustore_pages_insert_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::pages;
    template <vendored_residency_t residency_>
        requires requires { &pages_verbs<residency_>::insert; }
    static constexpr auto verb = &pages_verbs<residency_>::insert;
};
template <>
struct verb_slot<ustore_pages_remove_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::pages;
    template <vendored_residency_t residency_>
        requires requires { &pages_verbs<residency_>::remove; }
    static constexpr auto verb = &pages_verbs<residency_>::remove;
};
template <>
struct verb_slot<ustore_pages_move_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::pages;
    template <vendored_residency_t residency_>
        requires requires { &pages_verbs<residency_>::move; }
    static constexpr auto verb = &pages_verbs<residency_>::move;
};
template <>
struct verb_slot<ustore_pages_splice_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::pages;
    template <vendored_residency_t residency_>
        requires requires { &pages_verbs<residency_>::splice; }
    static constexpr auto verb = &pages_verbs<residency_>::splice;
};
template <>
struct verb_slot<ustore_pages_set_attributes_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::pages;
    template <vendored_residency_t residency_>
        requires requires { &pages_verbs<residency_>::set_attributes; }
    static constexpr auto verb = &pages_verbs<residency_>::set_attributes;
};
template <>
struct verb_slot<ustore_pages_remove_attributes_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::pages;
    template <vendored_residency_t residency_>
        requires requires { &pages_verbs<residency_>::remove_attributes; }
    static constexpr auto verb = &pages_verbs<residency_>::remove_attributes;
};
template <>
struct verb_slot<ustore_pages_read_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::pages;
    template <vendored_residency_t residency_>
        requires requires { &pages_verbs<residency_>::read; }
    static constexpr auto verb = &pages_verbs<residency_>::read;
};
template <>
struct verb_slot<ustore_pages_scan_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::pages;
    template <vendored_residency_t residency_>
        requires requires { &pages_verbs<residency_>::scan; }
    static constexpr auto verb = &pages_verbs<residency_>::scan;
};
template <>
struct verb_slot<ustore_strings_sample_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::strings;
    template <vendored_residency_t residency_>
        requires requires { &strings_verbs<residency_>::sample; }
    static constexpr auto verb = &strings_verbs<residency_>::sample;
};
template <>
struct verb_slot<ustore_strings_measure_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::strings;
    template <vendored_residency_t residency_>
        requires requires { &strings_verbs<residency_>::measure; }
    static constexpr auto verb = &strings_verbs<residency_>::measure;
};
template <>
struct verb_slot<ustore_docs_measure_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::docs;
    template <vendored_residency_t residency_>
        requires requires { &docs_verbs<residency_>::measure; }
    static constexpr auto verb = &docs_verbs<residency_>::measure;
};
template <>
struct verb_slot<ustore_graph_sample_vertices_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::graph;
    template <vendored_residency_t residency_>
        requires requires { &graph_verbs<residency_>::sample_vertices; }
    static constexpr auto verb = &graph_verbs<residency_>::sample_vertices;
};
template <>
struct verb_slot<ustore_graph_measure_vertices_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::graph;
    template <vendored_residency_t residency_>
        requires requires { &graph_verbs<residency_>::measure_vertices; }
    static constexpr auto verb = &graph_verbs<residency_>::measure_vertices;
};
template <>
struct verb_slot<ustore_pages_sample_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::pages;
    template <vendored_residency_t residency_>
        requires requires { &pages_verbs<residency_>::sample; }
    static constexpr auto verb = &pages_verbs<residency_>::sample;
};
template <>
struct verb_slot<ustore_pages_measure_t> {
    template <vendored_residency_t residency_>
    static constexpr auto table = &residency_tables<residency_>::pages;
    template <vendored_residency_t residency_>
        requires requires { &pages_verbs<residency_>::measure; }
    static constexpr auto verb = &pages_verbs<residency_>::measure;
};

/** Whether a call's verb sits in the control plane's table, served on host executors alone. */
template <typename arguments_type_>
concept a_control_plane_call = requires { verb_slot<arguments_type_>::database; };

/** Whether a call's verb sits in its modality's table at @p residency_. */
template <typename arguments_type_, vendored_residency_t residency_>
concept a_residency_call = requires { verb_slot<arguments_type_>::template verb<residency_>; };

#pragma endregion Verb Slots

#pragma region Serving

/** Hands @p visitor the first stream of @p view's executor, as <tt>visitor(stream)</tt> for a VRAM
 *  executor, whose claim it holds for the whole call, and <tt>visitor()</tt> for every host one, so a
 *  verb takes its residency's stream as its last argument; a filtered scan reaches the executor's
 *  other streams through @p view. */
template <typename visitor_type_>
util::status_t visit_residency(view_t const& view, visitor_type_&& visitor) noexcept {
    ustore_executor_t const executor = view.executor;
    if (!executor) return visitor();
    return std::visit(
        [&]<typename kind_type_>(kind_type_& kind) noexcept -> util::status_t {
            if constexpr (util::a_gpu_executor<kind_type_>) {
                auto [claim, claim_status] = kind.claim();
                if (claim_status.failed()) return claim_status;
                return visitor(kind.stream(0));
            }
            else return visitor();
        },
        executor->device);
}

/** Calls @p verb on @p arguments with every ragged input gathered on the host into the scratch
 *  @p view works in, then @p view and the executor of the verb's residency. */
template <typename verb_type_, typename arguments_type_, typename... streams_types_>
util::status_t call_gathered(verb_type_ verb, arguments_type_ const& arguments, view_t const& view,
                             streams_types_&... executor) noexcept {
    if constexpr (a_call_with_ragged_inputs<arguments_type_>) {
        auto const inputs = ragged_inputs(arguments);
        std::array<strings_arg_t, inputs.size()> strings;
        for (std::size_t index = 0; index != inputs.size(); ++index)
            if (util::status_t const status = gather_strings(inputs[index], view.scratch, strings[index]);
                status.failed())
                return status;
        return std::apply(
            [&](auto const&... gathered) noexcept { return verb(arguments, gathered..., view, executor...); }, strings);
    }
    else return verb(arguments, view, executor...);
}

/** Calls @p verb on @p arguments as @c call_gathered does, moving the inputs of a device call to the host first. */
template <typename verb_type_, typename arguments_type_, typename... streams_types_>
util::status_t call_staged(verb_type_ verb, arguments_type_ const& arguments, view_t const& view,
                           streams_types_&... executor) noexcept {
    if constexpr (sizeof...(streams_types_) != 0) {
        arguments_type_ staged = arguments;
        if (util::status_t const status = stage_inputs(gpu_drivers(), executor..., staged); status.failed())
            return status;
        return call_gathered(verb, staged, view, executor...);
    }
    else return call_gathered(verb, arguments, view);
}

/** Readies @p executor for one call of @p arguments: binds its device to the calling thread, rewinds the bounce and
 *  the managed runs, as every call before was waited for, and rewinds the call's arena, freeing its older runs in
 *  stream order. */
template <typename arguments_type_, util::a_gpu_stream executor_type_>
util::status_t open_device_call(arguments_type_ const& arguments, executor_type_& executor) noexcept {
    if (util::status_t const status = executor.bind(); status.failed()) return status;
    executor.memory.bounce.rewind();
    util::rewind(executor.memory.managed, executor);
    if constexpr (a_call_filling_arena<arguments_type_>)
        if (util::device_blocks_t* const blocks = util::device_blocks_of(arguments.arena, executor))
            util::rewind(*blocks, executor);
    return {};
}

/** Rewinds every scratch the call @p runner serves may fill, each pool thread's and the call's own,
 *  as slices keep their scratch from one fork to the next, or the one made for a call alone, and
 *  seeds each at @p bytes_per_thread, refusing a seed past the executor's @c scratch_limit. */
inline util::status_t reserve_scratches(view_t const& runner, std::size_t bytes_per_thread) noexcept {
    if (!runner.executor) return util::reserve(*util::host_blocks_of(runner.scratch), bytes_per_thread);
    for (ustore_arena_handle_t& scratch : runner.executor->scratches)
        if (util::status_t const seeded = util::reserve(*util::host_blocks_of(&scratch), bytes_per_thread);
            seeded.failed())
            return seeded;
    return {};
}

/** Scans what @p match narrows for @p arguments at @p residency_, on the host or on a device. */
template <vendored_residency_t residency_, typename match_type_, typename arguments_type_, typename... rest_types_>
util::status_t scan_matched_on(residency_tables<vendored_residency_t::host_k> const& host, match_type_ const& match,
                               arguments_type_ const& arguments, rest_types_&... rest) noexcept {
    if constexpr (residency_ == vendored_residency_t::host_k) return scan_matched(host, match, arguments, rest...);
    else return scan_matched_on_device(host, match, arguments, rest...);
}

/** Serves @p arguments on the control-plane verb of @p engine in @p view, holding the database for
 *  the transaction or snapshot an init opens. */
template <typename arguments_type_>
util::status_t serve_control(engine_t const& engine, arguments_type_ const& arguments, view_t const& view) noexcept {
    auto const verb = engine.database->*verb_slot<arguments_type_>::database;
    if (!verb) return util::status_t {util::status_code_t::unsupported_k, "The engine does not implement this call"};
    if constexpr (std::is_same_v<arguments_type_, ustore_transaction_init_t>) {
        ustore_transaction_t const reused = arguments.transaction ? *arguments.transaction : nullptr;
        if (reused && reused->database != arguments.database)
            return util::status_t {util::status_code_t::invalid_argument_k,
                                   "The transaction belongs to another database"};
        util::status_t const status = call_staged(verb, arguments, view);
        if (!status.failed() && !reused) hold(arguments.database, *arguments.transaction);
        return status;
    }
    else if constexpr (std::is_same_v<arguments_type_, ustore_snapshot_init_t>) {
        util::status_t const status = call_staged(verb, arguments, view);
        if (!status.failed()) hold(arguments.database, *arguments.snapshot);
        return status;
    }
    else if constexpr (std::is_same_v<arguments_type_, ustore_changes_cursor_init_t>)
        return open_changes_cursor(engine, arguments, view);
    else return call_staged(verb, arguments, view);
}

/** Serves @p arguments on the verb of their modality's table at @p residency_ in @p view, scanning
 *  through their match in the scratch @p runner names wherever it narrows the keys. */
template <vendored_residency_t residency_, typename arguments_type_, typename... streams_types_>
util::status_t serve_modality(engine_t const& engine, ustore_database_t database, arguments_type_ const& arguments,
                              view_t const& view, view_t const& runner, streams_types_&... executor) noexcept {
    auto const* const verbs = engine.template tables<residency_>().*
                              verb_slot<arguments_type_>::template table<residency_>;
    if (!verbs)
        return util::status_t {util::status_code_t::unsupported_k, "The engine keeps no collections of that modality"};
    auto const verb = verbs->*verb_slot<arguments_type_>::template verb<residency_>;
    if (!verb) return util::status_t {util::status_code_t::unsupported_k, "The engine does not implement this call"};
    if constexpr (a_call_with_match<arguments_type_>) {
        auto [match, match_status] = compile_match(arguments, runner.scratch, database->ceiling.strings, executor...);
        if (match_status.failed()) return match_status;
        if (!match.matches_everything())
            return with_call_stamp(engine, database, one_collection(arguments), view,
                                   [&](view_t const& pinned) noexcept {
                                       return call_staged(
                                           [&](arguments_type_ const& matched, auto&... rest) noexcept {
                                               return scan_matched_on<residency_>(engine.host, match, matched, rest...);
                                           },
                                           arguments, pinned, executor...);
                                   });
    }
    // A scan of documents called through the C ABI lists keys alone
    if constexpr (std::is_same_v<arguments_type_, ustore_docs_scan_t> && residency_ == vendored_residency_t::host_k)
        return call_staged([verb](ustore_docs_scan_t const& scan,
                                  view_t const& scan_view) noexcept { return verb(scan, nullptr, scan_view); },
                           arguments, view);
    else return call_staged(verb, arguments, view, executor...);
}

/** Serves @p arguments on the engine of their database, in the scratch and on the executor thread
 *  @p runner names, through the verb table of its executor's residency, refusing what no engine may
 *  see, and handing every ragged input to the engine readable in any layout; a pull is served by
 *  its own cursor. */
template <typename arguments_type_>
util::status_t serve(arguments_type_ const& arguments, view_t const& runner) noexcept {
    ustore_database_t const database = database_of(arguments);
    if (!database) return {util::status_code_t::uninitialized_k, "The database is uninitialized"};
    engine_t const& engine = engine_of(database);
    if constexpr (changes_database<arguments_type_>)
        if (database->access == access_t::read_only_k)
            return {util::status_code_t::read_only_k, "The database was opened read-only"};
    if constexpr (a_cursor_walker<arguments_type_>)
        if (arguments.cursor->walk.database != database)
            return {util::status_code_t::invalid_argument_k, "The cursor walks another database"};
    if constexpr (a_call_naming_collections<arguments_type_>)
        if (arguments.count && !arguments.collections.data)
            return {util::status_code_t::invalid_argument_k, "No collections were named"};
    if constexpr (std::is_same_v<arguments_type_, ustore_database_clear_t>)
        if (arguments.mode < ustore_drop_collection_k || arguments.mode > ustore_drop_values_k)
            return {util::status_code_t::invalid_argument_k, "Unknown drop mode"};
    if constexpr (outlasted_by_ingests<arguments_type_>)
        if (util::status_t const refused = refuse_open_ingests(*database); refused.failed()) return refused;
    if constexpr (std::is_same_v<arguments_type_, ustore_collection_create_t>) {
        if (arguments.modality < ustore_modality_blobs_k || arguments.modality > ustore_modality_pages_k)
            return {util::status_code_t::invalid_argument_k, "Unknown modality"};
        if (arguments.name.length && !arguments.name.data)
            return {util::status_code_t::invalid_argument_k, "A name with a length needs its bytes"};
        if (arguments.name.length && arguments.name.data[0] == '_')
            return {util::status_code_t::invalid_argument_k, "A leading underscore marks system collections"};
    }

    view_t view = runner;
    if (!decode_call(arguments, view))
        return {util::status_code_t::invalid_argument_k, "The view was made by neither view constructor"};
    if ((view.transaction && view.transaction->database != database) ||
        (view.snapshot && view.snapshot->database != database))
        return {util::status_code_t::invalid_argument_k, "The view reads another database"};

    if constexpr (a_call_filling_arena<arguments_type_>) util::arena_rewind(arguments.arena);
    if (util::status_t const seeded = reserve_scratches(runner, 0); seeded.failed()) return seeded;
    return visit_residency(
        view, [&]<typename... streams_types_>(streams_types_&... executor) noexcept -> util::status_t {
            constexpr vendored_residency_t residency_k = vendored_residency_of<streams_types_...>;
            if constexpr (residency_k != vendored_residency_t::host_k)
                if (util::status_t const opened = open_device_call(arguments, executor...); opened.failed())
                    return opened;
            if constexpr (a_cursor_walker<arguments_type_>)
                return with_call_stamp(engine, database, &arguments.cursor->walk.collection, view,
                                       [&](view_t const& pinned) noexcept {
                                           return pull(engine, arguments, *arguments.cursor, pinned, executor...);
                                       });
            else if constexpr (residency_k != vendored_residency_t::host_k &&
                               !a_residency_call<arguments_type_, residency_k>)
                return util::status_t {util::status_code_t::unsupported_k,
                                       "Changes and the control plane run on host executors"};
            else if constexpr (a_control_plane_call<arguments_type_>) return serve_control(engine, arguments, view);
            else return serve_modality<residency_k>(engine, database, arguments, view, runner, executor...);
        });
}

#pragma endregion Serving

#pragma region Routing

/** Whether deferred calls on @p executor are delegated to its pool thread 0: a caller-exclusive
 *  pool has one of its own, while an inclusive one, as a VRAM executor's is, runs on its caller. */
inline bool delegates(ustore_executor_handle_t const& executor) noexcept {
    return executor.threads.caller_exclusivity() == fu::caller_exclusive_k;
}

/** Waits until the dispatch of the last call delegated on @p executor has left pool thread 0, or
 *  returns at once on a pool that never delegates. */
inline void join_delegated(ustore_executor_handle_t& executor) noexcept {
    if (delegates(executor)) executor.threads.unsafe_join(0);
}

/** Takes the claim of @p executor once no call holds it, then joins the slice thread 0 may still be
 *  leaving, so the caller alone joins, parks or frees its pool. */
inline void take_idle_claim(ustore_executor_handle_t& executor) noexcept {
    while (!take(executor.holding_call.claim)) host_verbs().wait_while_claimed(&executor.holding_call.claim);
    join_delegated(executor);
}

/** Whether the calling thread serves the body of the call holding @p executor, so waiting on the
 *  executor would wait on itself. */
inline bool runs_holding_call(ustore_executor_handle_t& executor) noexcept {
    floor_atomic_ref<std::thread::id> const body_thread {executor.holding_call.body_thread};
    return body_thread.load(std::memory_order_relaxed) == std::this_thread::get_id();
}

/** The view the call holding @p executor serves in: its own scratch and index, past the pool's. */
inline view_t call_view(ustore_executor_handle_t& executor) noexcept {
    std::size_t const thread_index = executor.scratches.size() - 1;
    util::nvme_queues_t* const queues = executor.blocking_queues.threads() ? &executor.blocking_queues : nullptr;
    return view_t {.scratch = &executor.scratches[thread_index],
                   .executor = &executor,
                   .queues = queues,
                   .thread_index = thread_index};
}

/** Refuses @p executor, or its absence, on a database admitting @p admission, before the router
 *  claims it. */
inline util::status_t admit_executor(executor_admission_t admission,
                                     ustore_executor_handle_t const* executor) noexcept {
    if (admission == executor_admission_t::any_executor_k) return {};
    if (!executor)
        return admission == executor_admission_t::nvme_executor_k
                   ? util::status_t {util::status_code_t::invalid_argument_k,
                                     "An SPDK database call names no NVMe executor"}
                   : util::status_t {};
    if (std::holds_alternative<util::nvme_queues_t>(executor->device)) return {};
    bool const computes_on_device = !std::holds_alternative<std::monostate>(executor->device);
    if (computes_on_device && admission == executor_admission_t::no_ram_executor_k) return {};
    return {util::status_code_t::device_k, "An NVMe database needs an NVMe executor"};
}

/** Refuses the executor @p arguments name for the database they address, by what that database
 *  admits. */
template <typename arguments_type_>
util::status_t admit_executor(arguments_type_ const& arguments) noexcept {
    ustore_database_t const database = database_of(arguments);
    return database ? admit_executor(database->admitted_executors, arguments.executor) : util::status_t {};
}

/** An init names no database yet, so an NVMe open admits what a database on files does, and every
 *  other open any. */
template <typename arguments_type_>
    requires one_of<arguments_type_, ustore_database_init_ram_t, ustore_database_open_ram_t,
                    ustore_database_open_rocksdb_t, ustore_database_view_rocksdb_t, ustore_database_open_leveldb_t,
                    ustore_database_open_nvme_t, ustore_database_view_nvme_t>
util::status_t admit_executor(arguments_type_ const& arguments) noexcept {
    if constexpr (one_of<arguments_type_, ustore_database_open_nvme_t, ustore_database_view_nvme_t>)
        return admit_executor(executor_admission_t::no_ram_executor_k, arguments.executor);
    else return {};
}

/** Ends the call holding @p executor: its claims and handle counts, the executor claim, then its
 *  status. Once the claim is free nothing here reads the executor, as a free may delete it, and the
 *  next call joins the dispatch of a delegated one before using the pool. */
inline void end_call(ustore_executor_handle_t& executor) noexcept {
    holding_call_t& call = executor.holding_call;
    ustore_outcome_t& outcome = *call.outcome;
    util::status_t const recorded = call.recorded;
    release(call.claims);
    lower_named_calls(call.named_calls);
    floor_atomic_ref<std::thread::id> {call.body_thread}.store(std::thread::id {}, std::memory_order_relaxed);
    release(call.claim);
    finish(outcome, recorded);
}

/** Serves @p arguments in the scratch and on the thread @p runner names, leaving the outputs of a
 *  failed call null or zero, and answers the status their outcome is to finish with, which their
 *  claims are released together with. */
template <typename arguments_type_>
util::status_t serve_deferred(arguments_type_& arguments, view_t const& runner) noexcept {
    util::status_t const status = serve(arguments, runner);
    // A landed commit keeps its stamp beside a failed flush; the router zeroed it beforehand
    if constexpr (a_call_clearing_outputs<arguments_type_> && !a_call_stamping_commit<arguments_type_>)
        if (status.failed()) clear_outputs(arguments);
    return status;
}

/** Whether an open of @p arguments_type_ caps the CPU class, as the RAM and NVMe opens do. */
template <typename arguments_type_>
concept an_open_capping_cpu_class = requires(arguments_type_ const& arguments) { arguments.cpu_class_ceiling; };

/** Whether an open keeps the directory it names for later calls, as the RAM image and the RocksDB
 *  and LevelDB files are reopened by path; an NVMe open names a manifest or a raw medium. */
template <typename arguments_type_>
constexpr bool keeps_directory = one_of<arguments_type_, ustore_database_open_ram_t, ustore_database_open_rocksdb_t,
                                        ustore_database_view_rocksdb_t, ustore_database_open_leveldb_t>;

/** Opens through @p open_ with the directory of @p call resolved from the root once, so a relative
 *  one reaches the same files as its absolute spelling for the life of the handle. */
template <auto open_, typename arguments_type_>
util::status_t open_from_root(arguments_type_& call, cpu_ceiling_t const& ceiling, view_t const& runner) noexcept {
    if constexpr (!keeps_directory<arguments_type_>) return open_(call, ceiling, runner);
    else {
        if (!call.directory.length) return open_(call, ceiling, runner);
        char absolute[util::path_max_bytes_k];
        std::size_t length = 0;
        std::string_view const named = util::chars_of(call.directory);
        if (util::status_t const resolved = util::absolute_path_of(named, absolute, length); resolved.failed())
            return resolved;
        arguments_type_ rooted = call;
        rooted.directory = {reinterpret_cast<std::uint8_t const*>(absolute), length};
        return open_(rooted, ceiling, runner);
    }
}

/** Opens the database @p call names through @p open_, its engine's open, which claims nothing,
 *  under the ceiling its @c cpu_class_ceiling narrows, kept by the handle for every later call. */
template <typename arguments_type_, auto open_>
util::status_t serve_open(arguments_type_& call, view_t const& runner) noexcept {
    ustore_cpu_class_t cpu_class_ceiling = ustore_cpu_class_unknown_k;
    if constexpr (an_open_capping_cpu_class<arguments_type_>) cpu_class_ceiling = call.cpu_class_ceiling;
    auto const [ceiling, pick_status] = pick_cpu_ceiling(cpu_class_ceiling);
    if (pick_status.failed()) return pick_status;
    if (util::status_t const opened = open_from_root<open_>(call, ceiling, runner); opened.failed()) return opened;
    (*call.database)->ceiling = ceiling;
    return {};
}

/** The function serving a call of @p call_type_ in the scratch and on the thread a view names. */
template <typename call_type_>
using server_of = util::status_t (*)(call_type_&, view_t const&) noexcept;

/** Serves @p call with @p serve_ in the scratch and on the thread @p runner names, then releases
 *  the claims it took and finishes its outcome. */
template <typename call_type_, server_of<call_type_> serve_>
void run(call_type_* call, std::array<claim_t*, 3> const& claims, view_t const& runner) noexcept {
    util::status_t const status = serve_(*call, runner);
    release(claims);
    finish(*call->outcome, status);
}

/** Runs @p call on the calling thread over a scratch made for it alone, and over a blocking queue
 *  set that opens only if the call reaches its data through queues, as a call without an executor
 *  keeping either needs: the scratch's first page lies on the stack, and the blocks it grows into
 *  and the queues are freed once the call is final. */
template <typename call_type_, server_of<call_type_> serve_>
void run_alone(call_type_* call, std::array<claim_t*, 3> const& claims) noexcept {
    util::nvme_queues_t queues;
    alignas(util::arena_alignment_k) util::byte_t page[util::arena_page_bytes_k];
    util::arena_block_t* const first = new (page)
        util::arena_block_t {nullptr, sizeof(page), sizeof(util::arena_block_t)};
    ustore_arena_handle_t scratch {.memory = util::host_blocks_t {.newest = first}};
    std::array<std::uint32_t*, 3> const named_calls = raise_named_calls(*call);
    run<call_type_, serve_>(call, claims, view_t {.scratch = &scratch, .queues = &queues});
    lower_named_calls(named_calls);
    util::arena_free_blocks(scratch, first);
}

/** Serves on pool thread 0 the delegated copy of the call holding the executor at @p context, then
 *  ends it; the other threads return at once, free for the forks the call makes. */
template <typename call_type_, server_of<call_type_> serve_>
void serve_delegated(void* context, std::size_t thread, std::size_t) noexcept {
    if (thread != 0) return;
    ustore_executor_handle_t& executor = *static_cast<ustore_executor_handle_t*>(context);
    holding_call_t& call = executor.holding_call;
    floor_atomic_ref<std::thread::id> {call.body_thread}.store(std::this_thread::get_id(), std::memory_order_relaxed);
    call.recorded = serve_(*std::get_if<call_type_>(&call.delegated_arguments), call_view(executor));
    end_call(executor);
}

/** Runs @p call on the calling thread, alone or holding its executor, or delegates it to the pool's
 *  thread 0, as its executor and completion ask; a call finding the executor held is refused. */
template <typename call_type_, server_of<call_type_> serve_>
void dispatch(call_type_* call, std::array<claim_t*, 3> const& claims) noexcept {
    ustore_executor_handle_t* const executor = call->executor;
    if (util::status_t const refused = admit_executor(*call); refused.failed()) return refuse(*call, claims, refused);
    if (!executor) return run_alone<call_type_, serve_>(call, claims);
    holding_call_t& holding = executor->holding_call;
    if (!take(holding.claim))
        return refuse(*call, claims, {util::status_code_t::contended_k, "Another call holds the executor"});
    join_delegated(*executor);
    holding.outcome = call->outcome;
    holding.named_calls = raise_named_calls(*call);
    holding.claims = claims;
    host_verbs().publish(call->outcome, ustore_status_deferred_k, std::memory_order_relaxed);
    if (call->completion == ustore_completion_blocking_k || !delegates(*executor)) {
        floor_atomic_ref<std::thread::id> {holding.body_thread}.store(std::this_thread::get_id(),
                                                                      std::memory_order_relaxed);
        holding.recorded = serve_(*call, call_view(*executor));
        return end_call(*executor);
    }
    holding.delegated_arguments.template emplace<call_type_>(*call);
    // Thread 0 may end the call before this returns, so the token stays local; joins pass zero
    fu_generation_t generation = 0;
    if (fu::failed(executor->threads.unsafe_for_threads(&serve_delegated<call_type_, serve_>, executor, generation)))
        holding.recorded = {util::status_code_t::contended_k, "The pool runs another dispatch"}, end_call(*executor);
}

/** Leaves the outputs of @p arguments null or zero and claims what they hold, then dispatches them
 *  to the verb their type names. */
template <typename arguments_type_>
void route(arguments_type_* arguments) noexcept {
    if constexpr (a_call_clearing_outputs<arguments_type_>) clear_outputs(*arguments);
    auto const [claims, take_status] = take_claims(*arguments);
    if (take_status.failed()) return finish(*arguments->outcome, take_status);
    dispatch<arguments_type_, &serve_deferred<arguments_type_>>(arguments, claims);
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
    if (residency_of(arguments->executor) != vendored_residency_t::host_k)
        return finish(*arguments->outcome, {util::status_code_t::device_k, "Databases open on host executors"});
    if constexpr (an_open_naming_directory<arguments_type_>)
        if (arguments->directory.length && !arguments->directory.data)
            return finish(*arguments->outcome,
                          {util::status_code_t::invalid_argument_k, "A directory with a length needs its bytes"});
    dispatch<arguments_type_, &serve_open<arguments_type_, open_>>(arguments, {});
}

#pragma endregion Routing

#pragma region Compaction Plans

/**
 *  @brief Refuses the compaction or the plan's finish @p arguments describe in the ruled order,
 *      before the router claims anything.
 *
 *  The order is no database, a plan slot without an executor to outlive it, a plan opened on
 *  another database, a database opened read-only, then an executor computing on a device. A finish
 *  naming no plan is refused once its database is known.
 */
template <typename arguments_type_>
util::status_t admit_compaction(arguments_type_ const& arguments, ustore_database_compact_plan_t plan) noexcept {
    if (!arguments.database) return {util::status_code_t::uninitialized_k, "The database is uninitialized"};
    if constexpr (std::is_same_v<arguments_type_, ustore_database_compact_plan_finish_t>)
        if (!plan) return {util::status_code_t::invalid_argument_k, "The plan must not be null"};
    if constexpr (std::is_same_v<arguments_type_, ustore_database_compact_t>)
        if (arguments.plan && !arguments.executor)
            return {util::status_code_t::invalid_argument_k, "A plan needs an executor that outlives it"};
    if (plan && plan->opened_on != arguments.database)
        return {util::status_code_t::invalid_argument_k, "The plan was opened on another database"};
    if (arguments.database->access == access_t::read_only_k)
        return {util::status_code_t::read_only_k, "The database was opened read-only"};
    if (residency_of(arguments.executor) != vendored_residency_t::host_k)
        return {util::status_code_t::unsupported_k, "Compactions run on host executors"};
    if constexpr (outlasted_by_ingests<arguments_type_>) return refuse_open_ingests(*arguments.database);
    return {};
}

/**
 *  @brief Serves a compaction admitted and claimed by the router: continues the live plan it names,
 *      opens one into the empty slot it names, or runs a plan of its own to the end and frees it,
 *      writing the phase where the call stopped.
 *
 *  Opening takes the database's compaction claim, which a live plan keeps until its finish and a
 *  call naming no plan until it returns, so a database admits one compaction at a time. A cancel
 *  heard at the barrier closing the plan answers success, since the plan had ended.
 */
inline util::status_t serve_compaction(ustore_database_compact_t& arguments, view_t const& runner) noexcept {
    ustore_database_t const database = arguments.database;
    database_verbs_t const& verbs = *engine_of(database).database;
    if (!verbs.compact_plan_open)
        return {util::status_code_t::unsupported_k, "The engine does not implement this call"};
    void* engine_plan = arguments.plan && *arguments.plan ? (*arguments.plan)->engine_plan : nullptr;
    if (!engine_plan) {
        if (!take(database->compaction))
            return {util::status_code_t::contended_k, "Another compaction holds the database"};
        if (util::status_t const seeded = reserve_scratches(runner, 0); seeded.failed())
            return release(database->compaction), seeded;
        auto const [plan, status] = verbs.compact_plan_open(arguments, runner);
        if (status.failed()) return release(database->compaction), status;
        engine_plan = plan;
        if (arguments.plan) {
            ustore_database_compact_plan_handle_t* const held = new (std::nothrow)
                ustore_database_compact_plan_handle_t {};
            if (!held) {
                verbs.compact_plan.free(engine_plan);
                release(database->compaction);
                return {util::status_code_t::out_of_memory_k, "No room for the plan"};
            }
            held->engine_plan = engine_plan;
            held->opened_on = database;
            *arguments.plan = held;
        }
    }
    progress_ticks_t ticks {.callback = arguments.progress, .state = arguments.progress_state};
    util::status_t ran = verbs.compact_plan.run(engine_plan, runner, ticks);
    ustore_phase_progress_t const reached = verbs.compact_plan.phase(engine_plan);
    if (ran.code == util::status_code_t::cancelled_k && reached.phase == ustore_phase_done_k) ran = {};
    if (arguments.phase) *arguments.phase = reached;
    if (arguments.plan) return ran;
    util::status_t const finished = ran.succeeded() || ran.code == util::status_code_t::cancelled_k
                                        ? verbs.compact_plan.finish(engine_plan)
                                        : ran;
    verbs.compact_plan.free(engine_plan);
    release(database->compaction);
    return finished.failed() ? finished : ran;
}

/** Serves a plan's finish admitted by the router: once no call naming the plan is in flight, closes
 *  its engine plan and frees it, whatever the close answers. */
inline util::status_t serve_compaction_finish(ustore_database_compact_plan_finish_t& arguments,
                                              view_t const&) noexcept {
    ustore_database_compact_plan_t const plan = arguments.plan;
    wait_named_calls(plan->named_calls);
    util::status_t const finished = engine_of(plan->opened_on).database->compact_plan.finish(plan->engine_plan);
    free_compact_plan(plan);
    return finished;
}

/** Routes a compaction: clears its phase output, refuses it in the ruled order before any claim,
 *  then dispatches it as every call is dispatched, counting the call on the live plan it names. */
inline void route_compaction(ustore_database_compact_t* arguments) noexcept {
    if (arguments->phase) *arguments->phase = {};
    ustore_database_compact_plan_t const plan = arguments->plan ? *arguments->plan : nullptr;
    if (util::status_t const refused = admit_compaction(*arguments, plan); refused.failed())
        return finish(*arguments->outcome, refused);
    auto const [claims, take_status] = take_claims(*arguments);
    if (take_status.failed()) return finish(*arguments->outcome, take_status);
    dispatch<ustore_database_compact_t, &serve_compaction>(arguments, claims);
}

/** Routes a plan's finish as a compaction is routed, freeing the plan whatever the call answers. */
inline void route_compaction_finish(ustore_database_compact_plan_finish_t* arguments) noexcept {
    if (util::status_t const refused = admit_compaction(*arguments, arguments->plan); refused.failed())
        return refuse(*arguments, {}, refused);
    dispatch<ustore_database_compact_plan_finish_t, &serve_compaction_finish>(arguments, {});
}

#pragma endregion Compaction Plans

#pragma region Cursor Lifetimes

/** Opens the engine of a walk over @p scan_type_ at @p residency_, compiling what @p arguments
 *  match once into @p compiled. */
template <vendored_residency_t residency_, template <vendored_residency_t> typename verbs_type_, typename scan_type_,
          typename arguments_type_>
util::expected<cursor_engines<verbs_type_>> open_cursor_residency(engine_t const& engine,
                                                                  arguments_type_ const& arguments,
                                                                  ustore_arena_t compiled) noexcept {
    verbs_type_<residency_> const* const verbs = engine.template tables<residency_>().*
                                                 verb_slot<scan_type_>::template table<residency_>;
    if (!verbs || !(verbs->*verb_slot<scan_type_>::template verb<residency_>))
        return util::status_t {util::status_code_t::unsupported_k, "The engine keeps no collections of that modality"};
    auto [match, match_status] = compile_match(arguments, compiled, arguments.database->ceiling.strings);
    if (match_status.failed()) return match_status;
    if constexpr (residency_ == vendored_residency_t::host_k)
        return cursor_engines<verbs_type_> {cursor_engine<verbs_type_, residency_> {verbs, match}};
    else {
        // A walk outlives the call opening it, so its engine stays in the cursor's arena, and every
        // pull rebases it onto the managed runs that call rewinds
        compiled_match<residency_> on_device {match.matcher, match};
        on_device.host.matcher = match_everything_t {};
        return cursor_engines<verbs_type_> {cursor_engine<verbs_type_, residency_> {verbs, on_device}};
    }
}

/** Opens the engine of a walk over @p scan_type_ at the residency of @p executor. */
template <template <vendored_residency_t> typename verbs_type_, typename scan_type_, typename arguments_type_>
util::expected<cursor_engines<verbs_type_>> open_cursor_engine(engine_t const& engine,
                                                               ustore_executor_handle_t const* executor,
                                                               arguments_type_ const& arguments,
                                                               ustore_arena_t compiled) noexcept {
    switch (residency_of(executor)) {
    case vendored_residency_t::vram_nvidia_k:
        return open_cursor_residency<vendored_residency_t::vram_nvidia_k, verbs_type_, scan_type_>(engine, arguments,
                                                                                                   compiled);
    case vendored_residency_t::vram_amd_k:
        return open_cursor_residency<vendored_residency_t::vram_amd_k, verbs_type_, scan_type_>(engine, arguments,
                                                                                                compiled);
    case vendored_residency_t::vram_apple_k:
        return open_cursor_residency<vendored_residency_t::vram_apple_k, verbs_type_, scan_type_>(engine, arguments,
                                                                                                  compiled);
    default:
        return open_cursor_residency<vendored_residency_t::host_k, verbs_type_, scan_type_>(engine, arguments,
                                                                                            compiled);
    }
}

/** Opens a cursor of @p cursor_type_ over the walk @p arguments describe, holding their database
 *  and the transaction or snapshot their view reads through, at their executor's residency, and
 *  refusing a database whose engine lacks the verb serving @p scan_type_ there, a match that does
 *  not compile, or a view of another database. */
template <typename cursor_type_, template <vendored_residency_t> typename verbs_type_, typename scan_type_,
          typename arguments_type_>
util::expected<cursor_type_*> open_cursor(arguments_type_ const& arguments) noexcept {
    if (!arguments.cursor)
        return util::status_t {util::status_code_t::invalid_argument_k, "The call needs an output for the cursor"};
    *arguments.cursor = nullptr;
    ustore_database_t const database = arguments.database;
    if (!database) return util::status_t {util::status_code_t::uninitialized_k, "The database is uninitialized"};
    if (util::status_t const status = util::validate_direction(arguments.direction); status.failed()) return status;
    view_t view;
    if (!decode_view(arguments.view, view))
        return util::status_t {util::status_code_t::invalid_argument_k,
                               "The view was made by neither view constructor"};
    if (view.transaction ? view.transaction->database != database
                         : view.snapshot && view.snapshot->database != database)
        return util::status_t {util::status_code_t::invalid_argument_k, "The view reads another database"};

    cursor_type_* const cursor = new (std::nothrow) cursor_type_ {};
    if (!cursor) return util::status_t {util::status_code_t::out_of_memory_k, "No room for the cursor"};
    auto [opened, opened_status] = open_cursor_engine<verbs_type_, scan_type_>(engine_of(database), arguments.executor,
                                                                               arguments, &cursor->walk.compiled);
    if (opened_status.failed()) {
        util::arena_free_blocks(cursor->walk.compiled);
        delete cursor;
        return opened_status;
    }
    cursor->engine = opened;
    cursor->walk.database = database;
    cursor->walk.view = arguments.view;
    cursor->walk.collection = arguments.collection;
    cursor->walk.direction = arguments.direction;
    if constexpr (an_init_ending_at_key<arguments_type_>)
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
    wait_named_calls(cursor->walk.named_calls);
    view_t view;
    decode_view(cursor->walk.view, view);
    release_parent(view.transaction);
    release_parent(view.snapshot);
    release_hold(cursor->walk.database);
    util::arena_free_blocks(cursor->walk.compiled);
    delete cursor;
}

#pragma endregion Cursor Lifetimes

} // namespace unum::ustore

using namespace unum::ustore;

/** What @c ustore_blobs_cursor_t points to: a walk over the keys of one blobs collection, resumed
 *  at the key after the last one it handed out. */
struct ustore_blobs_cursor_handle_t {
    cursor_t walk;

    /** The first key in the walk's direction the next pull may hand out, or the end of the walk. */
    std::variant<ustore_key_t, exhausted_t> position;

    /** The key the walk stops before, if any. */
    std::optional<ustore_key_t> end_key;

    /** The verbs and the match of the residency the walk computes at. */
    cursor_engines<blobs_verbs> engine;
};

/** What @c ustore_docs_cursor_t points to: a walk over the keys of one documents collection,
 *  resumed as the blobs walk is. */
struct ustore_docs_cursor_handle_t {
    cursor_t walk;

    /** The first key in the walk's direction the next pull may hand out, or the end of the walk. */
    std::variant<ustore_key_t, exhausted_t> position;

    /** The key the walk stops before, if any. */
    std::optional<ustore_key_t> end_key;

    /** The verbs and the match of the residency the walk computes at. */
    cursor_engines<docs_verbs> engine;

    /** The columns every pull gathers, copied into @c walk.compiled at init. */
    docs_columns_t columns;
};

/** What @c ustore_pages_cursor_t points to: a walk over the keys of one pages collection, resumed
 *  as the blobs walk is. */
struct ustore_pages_cursor_handle_t {
    cursor_t walk;

    /** The first key in the walk's direction the next pull may hand out, or the end of the walk. */
    std::variant<ustore_key_t, exhausted_t> position;

    /** The key the walk stops before, if any. */
    std::optional<ustore_key_t> end_key;

    /** The verbs and the match of the residency the walk computes at. */
    cursor_engines<pages_verbs> engine;
};

/** What @c ustore_graph_vertices_cursor_t points to: a walk over the vertices of one graph
 *  collection, resumed as the blobs walk is. */
struct ustore_graph_vertices_cursor_handle_t {
    cursor_t walk;

    /** The first key in the walk's direction the next pull may hand out, or the end of the walk. */
    std::variant<ustore_key_t, exhausted_t> position;

    /** The key the walk stops before, if any. */
    std::optional<ustore_key_t> end_key;

    /** The verbs and the match of the residency the walk computes at. */
    cursor_engines<graph_verbs> engine;
};

/** What @c ustore_strings_cursor_t points to: a walk over the strings of one prefix, resumed at the
 *  string after the last one it handed out. */
struct ustore_strings_cursor_handle_t {

    /** What the walk holds and reads. */
    cursor_t walk;

    /** The prefix every string the walk hands out starts with. */
    sz::string_t prefix;

    /** The string the next pull starts at, or the end of the walk. */
    std::variant<sz::string_t, exhausted_t> position;

    /** Whether the next pull hands out the string it starts at, which a descending walk skips. */
    start_bound_t position_bound = start_bound_t::inclusive_k;

    /** The string the walk stops before, where an empty one bounds nothing. */
    sz::string_t end_string;

    /** The verbs and the match of the residency the walk computes at. */
    cursor_engines<strings_verbs> engine;
};

/** What @c ustore_changes_cursor_t points to: the claim of the pull filling it, the database and
 *  collection it reads, and its engine's state, which holds the pinned stamp and the position of
 *  the next pull. */
struct ustore_changes_cursor_handle_t {
    cursor_t walk;
    void* state = nullptr;
};
