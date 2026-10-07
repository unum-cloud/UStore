/**
 *  @file include/ustore/types.h
 *  @author Ash Vardanian
 *  @date June 12, 2022
 *  @brief Shared vocabulary of the UStore C interface: statuses, outcomes, arenas, progress
 *      callbacks, executors and the library version.
 *
 *  Everything the database and modality headers have in common, and nothing about a database,
 *  which @c db.h declares. Include @c ustore.h for every header, or a modality header alone, since
 *  each pulls this one in.
 *
 *  @section types_verbs Verbs
 *
 *  Every verb takes one argument structure named after it, as @c ustore_read takes a
 *  @c ustore_read_t, so optional arguments are skipped by name and their order never matters. Name
 *  lookups, views and the @c _free family stay positional. Every structure opens in the same order:
 *  the handle, @c executor, @c outcome, then @c completion. Inputs follow, led by the view of a read or the
 *  transaction of a write, then outputs and the @c arena they land in, then knobs, and last
 *  @c progress and @c progress_state. A verb that creates a handle names that output first.
 *
 *  Structures carry no size or version field, so they grow source-compatibly only: initialize them
 *  by designator, which zeroes every field left out, and expect the major version to change whenever
 *  one does. A @c NULL pointer only ever means its pointee's own default, and choices between
 *  behaviours are enums named for what they choose.
 *
 *  @section types_asynchrony Asynchrony
 *
 *  Synchronous and asynchronous calls share one shape, so no verb has an asynchronous twin. The
 *  argument structure is the whole request and the outcome is all the caller observes until the call
 *  ends, which lets a thread pool, a GPU stream, or a remote client serving the same header over the
 *  network sit behind the same verbs without changing a call site. Every call on an executor says how
 *  it completes: a blocking call, the default, returns with its final status stored and every output
 *  readable, and a call whose @c completion is @c ustore_completion_deferred_k returns once queued, as
 *  an io_uring entry does. A call with a @c NULL executor runs on the calling thread and is final when
 *  it returns, whatever its @c completion.
 *
 *  The outcome's status is the one word the library and the caller synchronize on. For a deferred
 *  call the library stores @c ustore_status_pending_k, then the message and the final status with
 *  release ordering, even for a call refused at submission, and touches nothing the structure names
 *  after that store. Device work stores them from a host callback that still fires after a device fault, so
 *  an outcome may live in any host memory. The caller reads the status with acquire ordering, through
 *  @c ustore_outcome_wait or by polling the field. Nothing is copied at submission, so the structure
 *  and every input, output, arena and executor it names stay borrowed until the status is final, and
 *  only then may the caller reuse the structure, free its buffers or pass its arena to another call.
 *
 *  Scheduling is bounded on both sides. Calls on one executor run in submission order, and a caller
 *  orders dependent calls itself, by waiting on the first before submitting the second. Each executor
 *  tracks a fixed number of deferred calls in flight, set by @c in_flight_limit, so a deferred
 *  submission that finds the table full finishes at once with @c ustore_status_contended_k instead of
 *  allocating or blocking, while blocking calls take no slot. An arena serves one call at a time. A
 *  call stops early only when its progress callback returns @c ustore_cancel_k, so
 *  @c ustore_status_pending_k is the only status a call in flight shows.
 *
 *  @code{.c}
 *  ustore_outcome_t outcomes[4] = {0};
 *  for (size_t scan = 0; scan != 4; ++scan) {
 *      scans[scan].executor = executor, scans[scan].outcome = &outcomes[scan];
 *      scans[scan].completion = ustore_completion_deferred_k;
 *      ustore_scan(&scans[scan]);
 *  }
 *  for (size_t scan = 0; scan != 4; ++scan) ustore_outcome_wait(&outcomes[scan]);
 *  @endcode
 *
 *  @section types_memory Memory
 *
 *  Every byte has one owner, and each owner picks where its bytes live once, when it is created:
 *
 *      Bytes                                Owner          Place chosen at
 *      Payload                              the handle     its init
 *      Scratch and staging                  the executor   its creation, grown to a high-water mark
 *      Outputs whose size a call discovers  the arena      the executor of the call that created it
 *      Outputs whose size the inputs bound  the caller     the caller's own span
 *
 *  Nothing about where memory comes from crosses the interface: databases and executors place
 *  theirs by the residency their constructor names, as @c ustore_database_init_nvme and
 *  @c ustore_executor_for_ram do. Outputs only a call can size, such as tapes of values, land in a
 *  @c ustore_arena_t the caller passes back to the next call, which rewinds and refills it, so a
 *  steady loop allocates once. An arena rewinds on every reuse and grows to its high-water mark,
 *  serves one call at a time, refuses inputs pointing into itself, and stays on the residency and
 *  device of the executor that created it, never falling back from VRAM to the host.
 *
 *  @code{.c}
 *  ustore_arena_t arena = NULL;
 *  for (size_t batch = 0; batch != batches_count; ++batch) {
 *      read.keys = keys + batch * read.count, read.values = &values, read.arena = &arena;
 *      ustore_read(&read);
 *      if (read.outcome->status != ustore_status_success_k) break; // blocking, so already final
 *      consume(values, read.offsets); // valid until the next call that takes `&arena`
 *  }
 *  ustore_arena_free(arena);
 *  @endcode
 *
 *  @section types_shapes Shapes
 *
 *  A verb's @c count is its batch axis, so a single operation is a batch of one. Inputs pass as
 *  arrays with byte strides, as BLAS does, so callers that pack arguments into arrays of structures
 *  pass them without copies, and a zero stride repeats the first value for every task. Outputs whose
 *  size the inputs bound, such as presences and offsets, land in the caller's spans, where a zero
 *  stride is refused, since every task would land in one place.
 *
 *  Ragged outputs are laid out as Apache Arrow expects: presence bits, @c count + 1 offsets whose
 *  last one closes the final value, and the bytes end to end. Ragged inputs address every value
 *  through a chunk pointer, a start inside that chunk and a length, each strided, so a tape read back
 *  is written again in place and a runtime object passes its bytes past its own header. Fields named
 *  @c offsets therefore always hold @c count + 1 entries, while @c starts hold one per value, and one
 *  more closing the last when the lengths are left out.
 */

#pragma once

#include <stddef.h> // `size_t`
#include <stdint.h> // `uint8_t`, `uint64_t`

/**
 *  @brief Marks every function the library exports, and nothing else.
 *
 *  On Windows it exports while CMake builds the shared @c ustore target, which defines @c ustore_EXPORTS, imports
 *  where a consumer of that shared library defines @c USTORE_SHARED, and expands to nothing for the static library.
 *  Elsewhere it gives the function default visibility, so it survives @c -fvisibility=hidden.
 */
#if !defined(USTORE_EXPORT)
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(ustore_EXPORTS)
#define USTORE_EXPORT __declspec(dllexport)
#elif defined(USTORE_SHARED)
#define USTORE_EXPORT __declspec(dllimport)
#else
#define USTORE_EXPORT
#endif
#else
#define USTORE_EXPORT __attribute__((visibility("default")))
#endif
#endif

/** The major version of UStore, which changes when the C ABI breaks. */
#define USTORE_VERSION_MAJOR 1

/** The minor version of UStore, which changes when the C ABI gains calls. */
#define USTORE_VERSION_MINOR 0

/** The patch version of UStore, which changes for fixes that keep the C ABI. */
#define USTORE_VERSION_PATCH 0

#ifdef __cplusplus
extern "C" {
#endif

#pragma region Scalars

/** An unsigned count or size, 64 bits wide on every platform. */
typedef uint64_t ustore_size_t;

/** A borrowed run of bytes and its length, never null-terminated by contract, so a caller holding a
 *  string without a terminator passes its own storage. */
typedef struct ustore_bytes_view_t {

    /** First byte of the run. */
    uint8_t const* data;

    /** Bytes the run spans, with no terminator assumed or written. */
    ustore_size_t length;

} ustore_bytes_view_t;

#pragma endregion Scalars

#pragma region Statuses and Outcomes

/** What went wrong, in one vocabulary shared by every engine and binding. */
typedef enum ustore_status_t {

    /** The call completed; @c message carries nothing. */
    ustore_status_success_k = 0,

    /** An allocation could not be satisfied. */
    ustore_status_out_of_memory_k = 1,

    /** An argument is null, contradicts another one, or carries a choice the call does not accept. */
    ustore_status_invalid_argument_k = 2,

    /** The call needs a database or transaction that was never opened. */
    ustore_status_uninitialized_k = 3,

    /** The engine or build does not implement what the call asks for. */
    ustore_status_unsupported_k = 4,

    /** A file or directory could not be opened, read, written, or closed. */
    ustore_status_io_k = 5,

    /** Stored or supplied data could not be decoded, like a corrupted file or an invalid document. */
    ustore_status_malformed_k = 6,

    /** A snapshot, collection, or document the call addresses does not exist. */
    ustore_status_not_found_k = 7,

    /** A snapshot, collection, or document the call creates already exists. */
    ustore_status_already_exists_k = 8,

    /** A concurrent writer changed a key this transaction wrote; retry the transaction. */
    ustore_status_write_conflict_k = 9,

    /** A concurrent writer changed a key this transaction read; retry the transaction. */
    ustore_status_read_conflict_k = 10,

    /** A concurrent writer inserted into a range this transaction scanned; retry the transaction. */
    ustore_status_phantom_conflict_k = 11,

    /** A hard design ceiling was reached, where a smaller request would fit. */
    ustore_status_capacity_exhausted_k = 12,

    /** A device could not be selected, or its memory could not be claimed, or the call and its arena
     *  belong to different residencies or devices. */
    ustore_status_device_k = 13,

    /** The engine failed in a way no other status names, which points at a defect. */
    ustore_status_internal_k = 14,

    /** The call was submitted and has not finished; never a final status. */
    ustore_status_pending_k = 15,

    /** The call found its executor's table of calls in flight full or its arena serving another call,
     *  or would wait on itself, like a drain from inside a progress callback of the same executor. */
    ustore_status_contended_k = 16,

    /** A progress callback stopped the call, which unwound and published nothing. */
    ustore_status_cancelled_k = 17,

} ustore_status_t;

/** Outcome of one call: a status to branch on, and a static message that is never freed and is @c NULL exactly
 *  when the status is @c ustore_status_success_k. */
typedef struct ustore_outcome_t {

    /** What went wrong, @c ustore_status_pending_k while the call runs, or @c ustore_status_success_k. */
    ustore_status_t status;

    /** Static detail the status cannot carry. */
    char const* message;

} ustore_outcome_t;

/**
 *  @brief Blocks until @p outcome is final, without spinning, and with acquire ordering.
 *  @param[in] outcome An outcome passed to a call, read until its status leaves @c ustore_status_pending_k.
 *  @return The final status, or @c ustore_status_pending_k at once when called from inside a callback of the
 *      executor running that call, rather than waiting on itself.
 */
USTORE_EXPORT ustore_status_t ustore_outcome_wait(ustore_outcome_t const* outcome);

/** Whether a call returns once its work is done, or once it is queued on the executor. */
typedef enum ustore_completion_t {

    /** Return with the final status stored and every output readable; the default. */
    ustore_completion_blocking_k = 0,

    /** Return once queued, with the status pending until the call ends; the caller waits or polls. */
    ustore_completion_deferred_k = 1,
} ustore_completion_t;

/** The canonical lowercase spelling of a status, such as "out-of-memory", in static storage. */
USTORE_EXPORT char const* ustore_status_name(ustore_status_t status);

/** Reads a status back from its canonical spelling, answering @c ustore_status_internal_k for a name no status
 *  carries. */
USTORE_EXPORT ustore_status_t ustore_status_from_name(ustore_bytes_view_t name);

#pragma endregion Statuses and Outcomes

#pragma region Arenas

/**
 *  @brief Memory the library allocates for the outputs only a call can size, reused across calls.
 *      @ref types_memory
 *
 *  A call given a pointer to a @c NULL arena creates one there. Given an existing one, it rewinds it,
 *  which invalidates every output of the previous call, and refills it, growing only when the new outputs
 *  outsize everything it held, so the arena settles at its high-water mark and a steady loop allocates
 *  once. To keep one call's outputs alive past the next call, pass the next call another arena. The
 *  memory lives where the executor of the call that created the arena computes, and an executor
 *  computing on a device that runs out of its memory reports @c ustore_status_out_of_memory_k rather
 *  than falling back to the host.
 *
 *  A call whose inputs point into the arena it is about to rewind reports
 *  @c ustore_status_invalid_argument_k. An arena serves one call at a time, so a second submission while
 *  the first is pending finishes with @c ustore_status_contended_k. A call on an executor of another
 *  residency or device than the arena's creator reports @c ustore_status_device_k. A call that fails or
 *  is cancelled leaves its outputs in the arena @c NULL.
 */
typedef struct ustore_arena_handle_t* ustore_arena_t;

/**
 *  @brief Frees an arena and every output in it, once the last call given it is final.
 *  @param[inout] arena The arena to be freed; @c NULL is safe.
 */
USTORE_EXPORT void ustore_arena_free(ustore_arena_t arena);

#pragma endregion Arenas

#pragma region Progress

/** Whether the call a progress callback watches carries on or unwinds. */
typedef enum ustore_continuation_t {

    /** Carry on to the next unit of work. */
    ustore_continue_k = 0,

    /** Stop, publish nothing, and report @c ustore_status_cancelled_k. */
    ustore_cancel_k = 1,

} ustore_continuation_t;

/**
 *  @brief Periodic report of a long call's advance, and the caller's only way to stop it.
 *
 *  Counts are in the units the verb consumes: tasks for a scan, bytes for an export or a compaction. The callback
 *  never runs concurrently with itself, however many threads do the work, and never after the call stores its final
 *  status. Its arguments are the units finished so far, never decreasing within one call, the units the call
 *  finishes if it runs to the end, and the @c progress_state registered beside it.
 */
typedef ustore_continuation_t (*ustore_progress_callback_t)(ustore_size_t completed, ustore_size_t total, void* state);

#pragma endregion Progress

#pragma region Executors

/**
 *  @brief An opaque handle to the threads or I/O rings that run calls, shared by any number of databases.
 *
 *  Calls on one executor run in the order they were submitted, while databases stay freely concurrent. An
 *  executor without threads of its own runs every call at submission, before returning. It owns the scratch and
 *  staging its calls need, placed where it computes and grown to a high-water mark, so a steady workload
 *  allocates once.
 *
 *  An executor tracks its deferred calls in a fixed table sized at creation, so submitting never allocates, and a
 *  deferred submission that finds the table full stores @c ustore_status_contended_k as its final status. From
 *  inside a progress callback of the executor running that call, @c ustore_outcome_wait answers
 *  @c ustore_status_pending_k at once and @c ustore_executor_drain reports @c ustore_status_contended_k, so nothing
 *  waits on itself.
 *  @c ustore_executor_free cannot report, so it is never called there.
 */
typedef struct ustore_executor_handle_t* ustore_executor_t;

/** What an executor's threads or rings do once it goes idle. */
typedef enum ustore_mood_t {

    /** Park them, so the next call pays a wake-up. */
    ustore_mood_park_k = 0,

    /** Keep them spinning, trading power for the next call's latency. */
    ustore_mood_spin_k = 1,

} ustore_mood_t;

/** The arguments of @c ustore_executor_for_ram. */
typedef struct ustore_executor_for_ram_t {

    /** Output for the new executor, which the caller frees with @c ustore_executor_free. */
    ustore_executor_t* executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_pending_k. */
    ustore_outcome_t* outcome;

    /** Threads to spawn, where zero takes every core. */
    ustore_size_t threads;

    /** Deferred calls the executor keeps in flight at once, sizing its table, where zero takes a default. */
    ustore_size_t in_flight_limit;

} ustore_executor_for_ram_t;

/**
 *  @brief Creates an executor of host threads for the RAM, RocksDB and LevelDB engines.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_executor_for_ram(ustore_executor_for_ram_t* arguments);

/** The arguments of @c ustore_executor_on_pool. */
typedef struct ustore_executor_on_pool_t {

    /** Output for the new executor, which the caller frees with @c ustore_executor_free. */
    ustore_executor_t* executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_pending_k. */
    ustore_outcome_t* outcome;

    /** The ForkUnion @c fu_pool_t to borrow, which outlives the executor and keeps its lender's mood. */
    void* pool;

    /** Deferred calls the executor keeps in flight at once, sizing its table, where zero takes a default. */
    ustore_size_t in_flight_limit;

} ustore_executor_on_pool_t;

/**
 *  @brief Creates an executor for the RAM, RocksDB and LevelDB engines over a ForkUnion pool it borrows, so several
 *      libraries in one process share one set of threads.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_executor_on_pool(ustore_executor_on_pool_t* arguments);

/** The arguments of @c ustore_executor_for_nvme. */
typedef struct ustore_executor_for_nvme_t {

    /** Output for the new executor, which the caller frees with @c ustore_executor_free. */
    ustore_executor_t* executor;

    /** Output for the outcome, final once its status leaves @c ustore_status_pending_k. */
    ustore_outcome_t* outcome;

    /** Threads to spawn, each owning one submission ring, where zero derives them from the cores. */
    ustore_size_t threads;

    /** Deferred calls the executor keeps in flight at once, sizing its table, where zero takes a default. */
    ustore_size_t in_flight_limit;

} ustore_executor_for_nvme_t;

/**
 *  @brief Creates an executor for the NVMe engine, whose every thread submits and polls its own I/O ring.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_executor_for_nvme(ustore_executor_for_nvme_t* arguments);

/** The arguments of @c ustore_executor_drain. */
typedef struct ustore_executor_drain_t {

    /** The executor to drain. */
    ustore_executor_t executor;

    /** Output for a failure of the executor itself, such as a lost device; every call reports its own. */
    ustore_outcome_t* outcome;

    /** What the threads do whenever the executor next goes idle, where the last drain wins. */
    ustore_mood_t mood;

} ustore_executor_drain_t;

/**
 *  @brief Waits for every call submitted before the drain began, so later submissions never delay it.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 */
USTORE_EXPORT void ustore_executor_drain(ustore_executor_drain_t* arguments);

/**
 *  @brief Drains an executor, then frees its threads, rings and scratch; a borrowed pool goes back to its lender.
 *  @param[inout] executor The executor to be freed; @c NULL is safe.
 */
USTORE_EXPORT void ustore_executor_free(ustore_executor_t executor);

#pragma endregion Executors

#pragma region Library Version

/**
 *  @brief Reports the version of the library the program runs against, which may differ from the macros it
 *      compiled against.
 *  @param[out] major Output for the major version.
 *  @param[out] minor Output for the minor version.
 *  @param[out] patch Output for the patch version.
 */
USTORE_EXPORT void ustore_version(int* major, int* minor, int* patch);

#pragma endregion Library Version

#ifdef __cplusplus
}
#endif
