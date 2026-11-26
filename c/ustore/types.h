/**
 *  @file c/ustore/types.h
 *  @author Ash Vardanian
 *  @date June 12, 2022
 *  @brief Shared vocabulary of the UStore C interface: statuses, outcomes, progress callbacks,
 *      executors, arenas, batches of strings and the library version.
 *
 *  Everything the database and modality headers have in common, and nothing about a database,
 *  which @c db.h declares. Include @c ustore.h for every header, or a modality header alone, since
 *  each pulls this one in.
 *
 *  @section types_verbs Verbs
 *
 *  Every verb that takes options, outputs, or callbacks takes one argument structure named
 *  after it, as @c ustore_blobs_read takes a @c ustore_blobs_read_t, so optional arguments
 *  are skipped by name and their order never matters. Name lookups, views, outcome polls
 *  and the @c _free family stay positional. Every structure opens in the same order: the
 *  handle, @c executor, @c outcome, then @c completion. The rest reads in the order the
 *  call works: what it reads, led by the view of a read or the transaction of a write, then
 *  the outputs and the @c written count of a listing right before the @c arena they land
 *  in, then the knobs that shape the work, such as @c wanted, a conversion of the outputs
 *  such as @c outputs_type, and @c mode, and last @c progress and @c progress_state. A verb
 *  that creates a handle names that output first.
 *
 *  Structures hold no size or version field, so they grow source-compatibly only: initialize them
 *  by designator, which zeroes every field left out, and expect the major version to change whenever
 *  one does. A @c NULL pointer only ever means its pointee's own default, apart from the outcome,
 *  and choices between behaviours are enums named for what they choose.
 *
 *  @section types_asynchrony Asynchrony
 *
 *  Every call names its @c completion, and blocking and deferred calls share one shape. A blocking
 *  call, the default, returns with its final status stored and every output readable.
 *
 *  A @c NULL executor runs the call on the calling thread. It creates no pool, thread, ring, SPDK
 *  queue pair or stream and probes no topology; its scratch and blocking queues live for that call
 *  only. Name an executor for calls that allocate nothing once warm. Such a call is final on
 *  return, whatever its completion.
 *
 *  An executor runs one call at a time. A call that finds another one holding its executor, from
 *  any thread, finishes @c ustore_status_contended_k without running. An Arrow export that
 *  converts an arena's outputs on a VRAM executor's device holds that executor while it converts,
 *  so a call the executor already accepted, deferred ones included, may finish
 *  @c ustore_status_contended_k then, without running.
 *
 *  A deferred call on a caller-exclusive executor returns once handed to the executor's thread 0,
 *  which runs it there, forks included, and finishes it, so polling alone sees it final. A deferred
 *  call on a caller-inclusive executor, whose threads need their caller, or on a VRAM executor,
 *  whose caller stages and feeds its devices, runs at submission and is final on return, except
 *  that a call an NVIDIA or AMD device serves through its host verb returns once its copies are
 *  queued, and its stream finishes it once they ran, with no thread of the library's. Submission
 *  copies the structure; everything it points to must stay allocated and unchanged until the call
 *  is final, and @c outcome->status reads @c ustore_status_deferred_k until then.
 *
 *  A call lets go of its arena and of the handles it names, then of its executor, and only then
 *  stores the message and the final status with release ordering. A caller that reads the status
 *  final with acquire ordering may reuse the arena, submit the next call or free any handle at
 *  once. Waits, drains and frees only wait: the executor's own threads finish every call. A free
 *  waits for the calls naming its handle, so one issued from a callback of such a call waits on
 *  itself. A call stops early only when its progress handler returns @c ustore_cancel_k.
 *
 *  One thread keeps several executors busy by giving each a deferred call and polling the statuses.
 *  A VRAM call is final when it returns, so a manager submits it last:
 *
 *  @code{.c}
 *  ustore_outcome_t ram_outcome = {0}, nvme_outcome = {0};
 *  ram_read.outcome = &ram_outcome, ram_read.completion = ustore_completion_deferred_k;
 *  nvme_read.outcome = &nvme_outcome, nvme_read.completion = ustore_completion_deferred_k;
 *  ustore_blobs_read(&ram_read);  // on a RAM executor
 *  ustore_blobs_read(&nvme_read); // on an NVMe executor
 *  while (ustore_outcome_status(&ram_outcome) == ustore_status_deferred_k ||
 *         ustore_outcome_status(&nvme_outcome) == ustore_status_deferred_k)
 *      prepare_next_batch();
 *  @endcode
 *
 *  @section types_memory Memory
 *
 *  Two complications shape memory, and nothing else may: outputs whose size only the backend
 *  knows, and memory a GPU should place itself. Arenas answer the first, executors the second, and
 *  every binding stays one verb per operation rather than a twin per output style. Every byte has
 *  one owner, and each owner picks where its bytes live once, when it is created.
 *
 *  The caller allocates every buffer a call reads or writes, and frees it after the call is final;
 *  no allocator callback crosses the interface, and every pointer a caller passes is an input,
 *  classified through the driver once per call.
 *  Payload belongs to the handle, placed by its constructor, as @c ustore_database_open_nvme places
 *  it on its drives. Scratch and staging belong to the executor, placed at its creation and grown
 *  to a high-water mark. Every output whose length scales with the call lands in an arena:
 *  values, presences, offsets, keys, identifiers, degrees, edges, bounds and columns alike. Only
 *  fixed-size reports, such as descriptions, stamps, the @c written count of a listing and created
 *  handles, stay caller fields, and a call that fails zeroes the scalars among them and leaves the
 *  handles @c NULL.
 *
 *  Executors make arenas. @c ustore_arena_init places one where its executor computes, decided
 *  once, and a @c NULL executor makes host pages for calls on the calling thread. A call rewinds
 *  the arena it is given, invalidating the previous call's outputs, and writes views into it, and
 *  any verb may reuse any arena its executor made, one call at a time. A blocking call uses the
 *  arena until it returns, and a deferred one until final. Shorthand with no place for an arena,
 *  such as @c db[key] or @c key @c in @c db, makes a one-off arena through @c ustore_arena_init and
 *  frees it with its result: slower than reuse, and stateless.
 *
 *  Outputs are views the call writes, so none holds a capacity or a stride for the caller to
 *  pick, and an arena keeps its high-water mark, so a steady loop allocates once. The library
 *  stages only to change a layout or to make pageable memory DMA-able.
 *
 *  @code{.c}
 *  ustore_arena_t arena = NULL;
 *  ustore_arena_init(&(ustore_arena_init_t){.arena = &arena, .executor = executor, .outcome = &outcome});
 *  for (size_t batch = 0; batch != batches_count; ++batch) {
 *      read.keys.data = keys + batch * read.count, read.values = &values, read.arena = arena;
 *      ustore_blobs_read(&read);
 *      if (read.outcome->status != ustore_status_success_k) break; // blocking, so already final
 *      consume(values); // valid until the next call given `arena`
 *  }
 *  ustore_arena_free(arena);
 *  @endcode
 *
 *  @section types_shapes Shapes
 *
 *  A verb's @c count is its batch axis, so a single operation is a batch of one. Every input takes
 *  one of three shapes. One run of bytes, a @c ustore_bytes_view_t, holds a path, a name or a
 *  request. A strided run of fixed-width elements, such as a @c ustore_keys_view_t, holds one
 *  element per task with a byte stride, as BLAS does, so callers that pack arguments into arrays of
 *  structures pass them without copies, and a zero stride repeats the first element for every task.
 *  A ragged batch, a @c ustore_strings_view_t, holds one string per task as an Apache Arrow tape of
 *  32-bit or 64-bit offsets, or through an accessor over any other shape, so a tape read back is
 *  written again as it is. A zeroed view of any shape stands for the input's default.
 *
 *  Outputs take three shapes too, written packed into the arena, so none holds a capacity or a
 *  stride. Fixed-width outputs are arrays, such as presence bits, keys, identifiers, degrees
 *  and bounds. Ragged bytes are a @c ustore_strings_view_t the call writes as a tape, with
 *  32-bit offsets while its bytes fit in 4 GiB and 64-bit beyond. Ragged keys are packed, split
 *  by task through @c count + 1 offsets, as every offset array holds one more entry than the
 *  runs it splits.
 */

#ifndef UNUM_USTORE_TYPES_H
#define UNUM_USTORE_TYPES_H

#include <stdint.h> // `uint8_t`, `uint32_t`, `uint64_t`

#pragma region Platform Identity

/*  Derived once from the OS predefines, which no other file reads and no build sets. */
#if defined(USTORE_OS_LINUX_) || defined(USTORE_OS_APPLE_) || defined(USTORE_OS_WINDOWS_) || \
    defined(USTORE_OS_FREEBSD_) || defined(USTORE_OS_POSIX_)
#error "USTORE_OS_*_ are identity, derived from the compiler; set a USTORE_WITH_* facility instead"
#endif

/** Whether the kernel is Linux, Android included, as 1 or 0. */
#if defined(__linux__)
#define USTORE_OS_LINUX_ 1
#else
#define USTORE_OS_LINUX_ 0
#endif

/** Whether the kernel is Darwin: macOS, iOS, tvOS, watchOS or visionOS, as 1 or 0. */
#if defined(__APPLE__)
#define USTORE_OS_APPLE_ 1
#else
#define USTORE_OS_APPLE_ 0
#endif

/** Whether the target is Windows, MinGW included and Cygwin not, as 1 or 0. */
#if defined(_WIN32)
#define USTORE_OS_WINDOWS_ 1
#else
#define USTORE_OS_WINDOWS_ 0
#endif

/** Whether the kernel is FreeBSD, as 1 or 0. */
#if defined(__FreeBSD__)
#define USTORE_OS_FREEBSD_ 1
#else
#define USTORE_OS_FREEBSD_ 0
#endif

/** Whether the POSIX interfaces exist: Linux, Darwin, FreeBSD or Cygwin, as 1 or 0. */
#if USTORE_OS_LINUX_ || USTORE_OS_APPLE_ || USTORE_OS_FREEBSD_ || defined(__CYGWIN__)
#define USTORE_OS_POSIX_ 1
#else
#define USTORE_OS_POSIX_ 0
#endif

#if USTORE_OS_LINUX_ + USTORE_OS_APPLE_ + USTORE_OS_WINDOWS_ + USTORE_OS_FREEBSD_ > 1
#error "Two operating-system identities hold at once"
#endif
#if USTORE_OS_POSIX_ && USTORE_OS_WINDOWS_
#error "POSIX and Windows identities hold at once"
#endif

#pragma endregion Platform Identity

#pragma region Facilities

/** Whether the in-tree NVMe engine is linked, as 1 or 0, which the library target passes on. */
#if !defined(USTORE_WITH_NVME)
#define USTORE_WITH_NVME 0
#endif

/** Whether the RocksDB engine is linked, as 1 or 0, which the library target passes on. */
#if !defined(USTORE_WITH_ROCKSDB)
#define USTORE_WITH_ROCKSDB 0
#endif

/** Whether the LevelDB engine is linked, as 1 or 0, which the library target passes on. */
#if !defined(USTORE_WITH_LEVELDB)
#define USTORE_WITH_LEVELDB 0
#endif

/** Whether SPDK's user-space NVMe driver is linked, as 1 or 0; its interface target passes it. */
#if !defined(USTORE_WITH_LINUX_SPDK)
#define USTORE_WITH_LINUX_SPDK 0
#endif
#if USTORE_WITH_LINUX_SPDK && !USTORE_OS_LINUX_
#error "USTORE_WITH_LINUX_SPDK needs Linux"
#endif

/** Whether this unit builds the shared library, as 1 or 0. */
#if !defined(USTORE_EXPORTING)
#define USTORE_EXPORTING 0
#endif

/** Whether this unit links the shared library rather than the archive, as 1 or 0. */
#if !defined(USTORE_SHARED)
#define USTORE_SHARED 0
#endif

/**
 *  @brief Marks every function the library exports, and nothing else.
 *
 *  On Windows it exports from the shared library, which builds with @c USTORE_EXPORTING, imports
 *  where a consumer of that library sets @c USTORE_SHARED, as linking @c ustore::shared does, and
 *  expands to nothing for the static library. GCC and Clang give the function default visibility,
 *  so it survives @c -fvisibility=hidden.
 */
#if !defined(USTORE_EXPORT)
#if USTORE_OS_WINDOWS_ && USTORE_EXPORTING
#define USTORE_EXPORT __declspec(dllexport)
#elif USTORE_OS_WINDOWS_ && USTORE_SHARED
#define USTORE_EXPORT __declspec(dllimport)
#elif USTORE_OS_WINDOWS_
#define USTORE_EXPORT
#elif defined(__GNUC__) || defined(__clang__)
#define USTORE_EXPORT __attribute__((visibility("default")))
#else
#define USTORE_EXPORT
#endif
#endif

#pragma endregion Facilities

/** The major version of UStore, which changes when the C ABI breaks. */
#define USTORE_VERSION_MAJOR 1

/** The minor version of UStore, which changes when the C ABI gains calls. */
#define USTORE_VERSION_MINOR 0

/** The patch version of UStore, which changes for fixes that keep the C ABI. */
#define USTORE_VERSION_PATCH 0

#ifdef __cplusplus
extern "C" {
#endif

/** An unsigned count or size, 64 bits wide on every platform. */
typedef uint64_t ustore_size_t;

/** A run of bytes the caller owns and its length, never null-terminated by contract, so a caller
 *  holding a string without a terminator passes its own storage. */
typedef struct ustore_bytes_view_t {

    /** First byte of the run. */
    uint8_t const* data;

    /** Bytes the run spans, with no terminator assumed or written. */
    ustore_size_t length;

} ustore_bytes_view_t;

/** Writes the view of every string <tt>indices[i]</tt> into <tt>views[i]</tt>, for strings no tape holds, and accepts
 *  any @p count. */
typedef void (*ustore_strings_get_t)(void const* handle, ustore_size_t const* indices, ustore_size_t count,
                                     ustore_bytes_view_t* views);

/** How a batch of strings holds its bytes, naming the member of its @c data union to read. */
typedef enum ustore_strings_layout_t {

    /** Apache Arrow @c Binary, where string @c i spans @b [offsets[i],offsets[i+1]) of the bytes. */
    ustore_strings_u32_tape_k = 0,

    /** Apache Arrow @c LargeBinary, the same with 64-bit offsets. */
    ustore_strings_u64_tape_k = 1,

    /** Strings found through a @c ustore_strings_get_t accessor, which inputs alone use. */
    ustore_strings_accessor_k = 2,

} ustore_strings_layout_t;

/**
 *  @brief A ragged batch of byte strings in one of three layouts: an Arrow tape of 32-bit or 64-bit offsets, or an
 *      accessor writing views over any other shape, like an array of runtime objects or of records.
 *
 *  Keys of a strings collection, binary values, documents, names and field paths all arrive in it. As an input it
 *  holds one string per task, or as many as its verb counts otherwise, and a zeroed view, a u32 tape with @c NULL
 *  offsets, stands for the input's default. The accessor runs on the thread that runs the call, in blocks of up to
 *  64 indices, never concurrently with itself and never after the call is final. As an output the call writes a
 *  tape, with 32-bit offsets while its bytes fit in 4 GiB and 64-bit beyond.
 */
typedef struct ustore_strings_view_t {

    /** Which member of @c data describes the strings. */
    ustore_strings_layout_t layout;

    /** The description of the strings, read through the member that @c layout names. */
    union {

        /** The accessor of a @c ustore_strings_accessor_k batch. */
        struct {

            /** Writes the views of the requested strings. */
            ustore_strings_get_t get;

            /** The state @c get reads, like an array of objects. */
            void const* handle;
        } accessor;

        /** The tape of a @c ustore_strings_u32_tape_k batch. */
        struct {

            /** The bytes of every string, back to back. */
            uint8_t const* data;

            /** Offsets into @c data, one per string and one past the end of the last string. */
            uint32_t const* offsets;
        } u32_tape;

        /** The tape of a @c ustore_strings_u64_tape_k batch. */
        struct {

            /** The bytes of every string, back to back. */
            uint8_t const* data;

            /** Offsets into @c data, one per string and one past the end of the last string. */
            uint64_t const* offsets;
        } u64_tape;
    } data;

} ustore_strings_view_t;

/**
 *  @brief What went wrong, in one vocabulary shared by every engine and binding.
 *
 *  Shared statuses hold USearch's numbers, 0 to 19, leaving 3, 4 and 5 unused, and UStore's own
 *  start at 20. Statuses fall into five categories: resources, input, state, interruptions and
 *  defects. Any verb that allocates, binds a device or touches a file may also return a resources
 *  status; any verb taking a progress callback may return an interruption. A verb's own block
 *  lists only what its contract adds.
 */
typedef enum ustore_status_t {

    /** The call succeeded; @c message holds nothing. */
    ustore_status_success_k = 0,

    /** A deferred call that has not finished; the one status that is never final. */
    ustore_status_deferred_k = 1,

    /** An allocation could not be satisfied. */
    ustore_status_out_of_memory_k = 2,

    /** The call would change a database opened read-only, as @c ustore_database_view_rocksdb opens one. */
    ustore_status_read_only_k = 6,

    /** A file or directory could not be opened, read, written, or closed. */
    ustore_status_io_k = 7,

    /** A device could not be selected, or its memory could not be claimed, or the call's executor, database and
     *  arena belong to different residencies or devices. */
    ustore_status_device_k = 8,

    /** Stored or supplied data could not be decoded, like a corrupted file, an invalid document, or
     *  raw NVMe data directories listed in another order than the database was created with. */
    ustore_status_malformed_k = 9,

    /** The engine failed in a way no other status names, which points at a defect. */
    ustore_status_internal_k = 10,

    /** A count passed what its word holds, like the open exports of one arena. */
    ustore_status_overflow_k = 11,

    /** The call needs a database or transaction that was never opened. */
    ustore_status_uninitialized_k = 12,

    /** The handles or arguments are each well-formed but cannot be joined, like devices of more
     *  than one vendor, or a stream on another device than its entry names. */
    ustore_status_config_mismatch_k = 13,

    /** The call was refused rather than waited for: another call holds its executor, arena, cursor
     *  or transaction, or a drain came from inside a callback of the call it would wait on. */
    ustore_status_contended_k = 14,

    /** A progress callback stopped the call, which unwound and published nothing. */
    ustore_status_cancelled_k = 15,

    /** The engine or build does not implement what the call asks for, like io_uring on a kernel without it. */
    ustore_status_unsupported_k = 16,

    /** An argument is null, contradicts another one, or holds a choice the call does not
     *  accept, like a collection of another modality, a name reserved for a system collection,
     *  or a batch of strings in an unknown layout. The outcome itself is never @c NULL: it is
     *  where this status lands. */
    ustore_status_invalid_argument_k = 17,

    /** A collection, a directory, or a document a modification needs does not exist, while a read marks a missing
     *  key with an unset presence bit instead. */
    ustore_status_not_found_k = 18,

    /** A collection name or a document the call creates is already taken, or the first slot of a
     *  raw medium an NVMe database opens belongs to something else. */
    ustore_status_already_exists_k = 19,

    /** A concurrent writer changed a key this transaction wrote; retry the transaction. */
    ustore_status_write_conflict_k = 20,

    /** A concurrent writer changed a key this transaction read; retry the transaction. */
    ustore_status_read_conflict_k = 21,

    /** A concurrent writer inserted into a range this transaction scanned; retry the transaction. */
    ustore_status_phantom_conflict_k = 22,

    /** A hard design ceiling was reached. */
    ustore_status_capacity_exhausted_k = 23,

} ustore_status_t;

/** Outcome of one call: the status word a deferred call completes through and every binding
 *  branches on, and a static message naming what broke, a library invariant or a rule of this
 *  call, else the status's own @ref ustore_status_name. Messages are never freed, and are
 *  @c NULL exactly when the status is @c ustore_status_success_k. */
typedef struct ustore_outcome_t {

    /** What went wrong, @c ustore_status_success_k, or @c ustore_status_deferred_k until a deferred call finishes. */
    ustore_status_t status;

    /** Static detail the status cannot hold. */
    char const* message;

} ustore_outcome_t;

/**
 *  @brief Blocks on an atomic wait until @p outcome is final, with acquire ordering.
 *  @param[in] outcome An outcome passed to a call, read until its status leaves @c ustore_status_deferred_k.
 *  @return The final status.
 *
 *  Never wait on a call's own outcome from inside its progress callback: the call finishes only after the callback
 *  returns.
 */
USTORE_EXPORT ustore_status_t ustore_outcome_wait(ustore_outcome_t* outcome);

/**
 *  @brief Reads the status of @p outcome without waiting, with the acquire ordering its final store pairs with, so a
 *      final status also publishes every output of the call.
 *  @param[in] outcome An outcome passed to a call.
 *  @return The final status once the call is final.
 *
 *  The one way to poll: the status word is atomic only by this contract, so a caller reading the field directly
 *  needs an atomic load its language may not offer on foreign memory.
 *
 *  @par Statuses:
 *  - @c ustore_status_deferred_k The call is not final.
 */
USTORE_EXPORT ustore_status_t ustore_outcome_status(ustore_outcome_t* outcome);

/** Whether a call returns final or once submitted to its executor. */
typedef enum ustore_completion_t {

    /** Return with the final status stored and every output readable; the default. */
    ustore_completion_blocking_k = 0,

    /** Return once submitted, with the status @c ustore_status_deferred_k until the call finishes; the caller
     *  waits or polls. */
    ustore_completion_deferred_k = 1,
} ustore_completion_t;

/** The canonical lowercase spelling of a status, such as "out-of-memory", in static storage, or "unrecognized" for
 *  a value no status holds. */
USTORE_EXPORT char const* ustore_status_name(ustore_status_t status);

/** Reads a status back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_status_from_name(ustore_bytes_view_t name, ustore_status_t* status);

/** Whether the call a progress callback watches continues or unwinds. */
typedef enum ustore_continuation_t {

    /** Continue to the next unit of work. */
    ustore_continue_k = 0,

    /** Stop, publish nothing, and report @c ustore_status_cancelled_k. */
    ustore_cancel_k = 1,

} ustore_continuation_t;

/**
 *  @brief Periodic report of a long call's advance, and the caller's only way to stop it.
 *
 *  Counts are in the units the verb consumes: tasks for a scan, one pull for a cursor, bytes of
 *  keys and values for a save, and bytes or collections for a compaction. The callback never runs
 *  concurrently with itself, however many threads do the work, and never after the call stores its
 *  final status. Its arguments are the units finished so far, never decreasing within one call, or
 *  within one phase for a plan's call, which reports at every barrier with that phase's units; the
 *  units the call or the phase finishes if it runs to the end; and its @c progress_state.
 */
typedef ustore_continuation_t (*ustore_progress_callback_t)(ustore_size_t finished, ustore_size_t total, void* state);

/** The kind of work a plan is doing, shared by every long operation: a compaction's flush, its
 *  merges one unit at a time, then its reclaim. */
typedef enum ustore_phase_t {

    /** No plan, or one that has not started. */
    ustore_phase_none_k = 0,

    /** Writing what memory holds to storage: memory runs on NVMe, memtables on RocksDB. */
    ustore_phase_flush_k = 1,

    /** Merging or vacuuming the stored versions one unit at a time: a collection, a column family
     *  or an NVMe round. */
    ustore_phase_merge_k = 2,

    /** Freeing what no reader reaches any more and rewriting the image that names the rest. */
    ustore_phase_reclaim_k = 3,

    /** The plan ran to its end, and nothing is pending. */
    ustore_phase_done_k = 4,

} ustore_phase_t;

/** The canonical spelling of a plan's phase, as @c ustore_phase_merge_k spells "merge", or
 *  "unrecognized" for a value no phase holds. */
USTORE_EXPORT char const* ustore_phase_name(ustore_phase_t phase);

/** Reads a plan's phase back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_phase_from_name(ustore_bytes_view_t name, ustore_phase_t* phase);

/** Where a plan stands: its phase, that phase's position in the plan, and its own count. */
typedef struct ustore_phase_progress_t {

    /** The kind of work in hand. */
    ustore_phase_t phase;

    /** This phase's position among the ones the plan runs, from zero. */
    ustore_size_t ordinal;

    /** Units of this phase done: collections, column families, rounds or bytes, as the engine
     *  counts them. */
    ustore_size_t completed;

    /** Units of this phase in all. */
    ustore_size_t total;

} ustore_phase_progress_t;

/** What the strings of a @c ustore_patterns_t are matched as. */
typedef enum ustore_pattern_kind_t {

    /** Regular expressions, of which a listed value matches at least one somewhere. */
    ustore_pattern_regex_k = 0,

    /** Literal substrings, of which a listed value holds at least one, found together through Aho-Corasick. */
    ustore_pattern_substrings_k = 1,

} ustore_pattern_kind_t;

/** The canonical spelling of a pattern kind, as @c ustore_pattern_regex_k spells "regex", or "unrecognized". */
USTORE_EXPORT char const* ustore_pattern_kind_name(ustore_pattern_kind_t kind);

/** Reads a pattern kind back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_pattern_kind_from_name(ustore_bytes_view_t name, ustore_pattern_kind_t* kind);

/** Whether patterns compare bytes as they are or after Unicode case folding. */
typedef enum ustore_case_sensitivity_t {

    /** Bytes as they are. */
    ustore_cased_k = 0,

    /** Full Unicode case folding of both the patterns and the values. */
    ustore_uncased_k = 1,

} ustore_case_sensitivity_t;

/** The canonical spelling of a case sensitivity, as @c ustore_uncased_k spells "uncased", or "unrecognized". */
USTORE_EXPORT char const* ustore_case_sensitivity_name(ustore_case_sensitivity_t sensitivity);

/** Reads a case sensitivity back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_case_sensitivity_from_name(ustore_bytes_view_t name,
                                                                ustore_case_sensitivity_t* sensitivity);

/**
 *  @brief Patterns a scan keeps the values or strings matching, shared by every task of the call and compiled once per
 *      call or once per cursor.
 *
 *  A zeroed structure matches everything. Patterns that fail to compile refuse the call
 *  with @c ustore_status_invalid_argument_k, holding StringZilla's message.
 */
typedef struct ustore_patterns_t {

    /** The patterns, where a zeroed view matches everything. */
    ustore_strings_view_t patterns;

    /** Number of strings in @c patterns. */
    ustore_size_t count;

    /** What the patterns are matched as. */
    ustore_pattern_kind_t kind;

    /** Whether matching folds case. */
    ustore_case_sensitivity_t sensitivity;

    /** Optional weight of every substring pattern, which turns on BM25 scores; refused with regexes. */
    float const* weights;

} ustore_patterns_t;

/**
 *  @brief An opaque handle to the threads or I/O rings that run calls, shared by any number of databases.
 *
 *  Its kind picks where calls compute: host threads for every engine, or one device for the reads
 *  and scans an engine computes there, where a change, the control plane and every call no device
 *  tower serves yet answer @c ustore_status_unsupported_k, and an arena or cursor of another kind
 *  of executor @c ustore_status_device_k. An executor serves one call at a time, and a second call
 *  finishes @c ustore_status_contended_k, while databases stay freely concurrent. A blocking call
 *  runs on the calling thread, and a deferred call is delegated to the first thread of a
 *  caller-exclusive pool, whose every fork inside the call splits it over the other threads. In a
 *  build without OS threads, an executor runs every call at submission, before returning. It owns
 *  the scratch and staging its calls need, placed where it computes and grown to a high-water mark,
 *  so a steady workload allocates once, and a RAM executor keeps the scratch of each thread on the
 *  memory domain local to that thread.
 *
 *  Every verb accepts a @c NULL executor and then runs on the calling thread, blocking whatever its
 *  @c completion says: RAM work runs single-threaded, VRAM verbs answer @c ustore_status_device_k,
 *  and NVMe reads block through the files the database already holds. A database reading through
 *  SPDK answers @c ustore_status_invalid_argument_k to a @c NULL executor, since no blocking read
 *  reaches its namespaces. Its outputs land in any arena of the handle's residency.
 *
 *  Every other call on an executor, from inside a progress callback of its own call, reports
 *  @c ustore_status_contended_k. A drain reports it only from the thread running the call's body,
 *  as a callback on another thread of a split batch is not told apart, so such a callback never
 *  drains its own executor, which would wait on itself. Frees cannot report, so neither
 *  @c ustore_executor_free nor the free of a handle the running call names is ever called from a
 *  callback of that call.
 *
 *  The library stages only to change layout on the side that computes, or to make pageable memory
 *  DMA-able, and never routes device bytes through the host:
 *
 *      Case                                          Action
 *      Device inputs on the executor's device        read in place
 *      Device inputs needing padding or a cast       a device-to-device copy or a cast kernel
 *      Pinned or managed host inputs                 a direct asynchronous upload
 *      Pageable host inputs                          the pinned bounce buffer, then an upload
 *      Outputs in an arena on the executor's device  the kernel writes them directly
 */
typedef struct ustore_executor_handle_t* ustore_executor_t;

/** What an executor's threads do between a drain and its next call. */
typedef enum ustore_mood_t {

    /** Sleep in the kernel until the next call, freeing the cores. */
    ustore_mood_parked_k = 0,

    /** Keep polling, trading the cores for the lowest wake-up latency. */
    ustore_mood_spinning_k = 1,

} ustore_mood_t;

/** The canonical spelling of a mood, as @c ustore_mood_spinning_k spells "spinning", or "unrecognized". */
USTORE_EXPORT char const* ustore_mood_name(ustore_mood_t mood);

/** Reads a mood back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_mood_from_name(ustore_bytes_view_t name, ustore_mood_t* mood);

/** The CPU generation a database or an executor confines itself to, named as the sibling libraries name it. */
typedef enum ustore_cpu_class_t {

    /** Picks the newest class this machine runs. */
    ustore_cpu_class_unknown_k = 0,

    // Portable

    /** The portable floor every CPU runs. */
    ustore_cpu_class_serial_k = 1,

    /** The one class of a monoversioned build, whatever its floor implies or its preset says. */
    ustore_cpu_class_monoversioned_k = 2,

    // x86

    /** x86 cores from Intel Haswell on. */
    ustore_cpu_class_intel_haswell_k = 3,

    /** x86 efficiency cores from Intel Sierra Forest on. */
    ustore_cpu_class_intel_sierra_k = 4,

    /** x86 server cores from Intel Skylake on. */
    ustore_cpu_class_intel_skylake_k = 5,

    /** x86 server cores from Intel Ice Lake on. */
    ustore_cpu_class_intel_icelake_k = 6,

    // Arm

    /** Arm cores from AWS Graviton 2 on. */
    ustore_cpu_class_aws_g2_k = 12,

    /** Arm cores from Apple M2 on. */
    ustore_cpu_class_apple_m2_k = 13,

    /** Arm server cores from AWS Graviton 5 on. */
    ustore_cpu_class_aws_g5_k = 15,

    // RISC-V

    /** RISC-V cores with RVV, Zawrs and Zacas, from Alibaba's C930 on. */
    ustore_cpu_class_alibaba_c930_k = 17,

    // Power

    /** Power cores with VSX, ISA 3.0 store atomics and @c pause_short waits, from POWER10 on. */
    ustore_cpu_class_ibm_power10_k = 18,

} ustore_cpu_class_t;

/** The canonical spelling of a CPU class, as @c ustore_cpu_class_intel_haswell_k spells "intel-haswell" and
 *  @c ustore_cpu_class_unknown_k spells "unknown", or "unrecognized". */
USTORE_EXPORT char const* ustore_cpu_class_name(ustore_cpu_class_t cpu_class);

/** Reads a CPU class back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_cpu_class_from_name(ustore_bytes_view_t name, ustore_cpu_class_t* cpu_class);

/**
 *  @brief The host memory domains a RAM executor serves, NUMA nodes as the operating system numbers them.
 *
 *  Names where the executor keeps memory and runs its threads, never where a database keeps its payload. The scratch
 *  of each thread lives on the domain local to that thread when the options name it, and on the first domain named
 *  otherwise, as does every arena the executor makes.
 */
typedef struct ustore_ram_options_t {

    /** Identifiers of the memory domains, copied by the call, where @c NULL derives every
     *  domain the process may allocate on, and an identifier no domain holds is refused
     *  with @c ustore_status_invalid_argument_k. */
    int32_t const* memory_domains;

    /** Entries of @c memory_domains, where zero derives as @c NULL does. */
    ustore_size_t memory_domains_count;

} ustore_ram_options_t;

/** The arguments of @c ustore_executor_for_ram. */
typedef struct ustore_executor_for_ram_t {

    /** Output for the new executor, which the caller frees with @c ustore_executor_free; @c NULL on failure. */
    ustore_executor_t* executor;

    /** Output for the outcome, final on return. */
    ustore_outcome_t* outcome;

    /** The memory domains the executor serves; copied at creation, so the caller may free it once
     *  the call returns. */
    ustore_ram_options_t ram;

    /** Threads to spawn, interleaved across the compute domains local to @c ram, where zero
     *  takes all but one in eight of the cores those domains hold, and at least one. */
    ustore_size_t threads;

    /** The newest CPU class the threads may confine their waits and atomics to; a class the CPU
     *  lacks narrows the pick rather than refusing it, and zero sets no ceiling. */
    ustore_cpu_class_t cpu_class_ceiling;

    /** The ceiling on each thread's scratch, which every call's staging carves from, past which a
     *  call answers @c ustore_status_out_of_memory_k; zero takes 16 GiB. */
    ustore_size_t scratch_limit;

} ustore_executor_for_ram_t;

/**
 *  @brief Creates an executor of host threads for the RAM, RocksDB and LevelDB engines.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  The default thread count leaves an eighth of the cores to blocking calls, which run on their
 *  callers' threads, and to the background flushes and compactions of RocksDB and LevelDB. The
 *  threads interleave across the compute domains local to the memory domains named, where ForkUnion
 *  spawns near them: every domain the process may allocate on, or one alone. Naming several but not
 *  all spreads them over the whole machine.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c executor output is @c NULL, a memory domain
 *    identifier names no domain, the ceiling names no CPU class, or ForkUnion refuses an argument.
 *  - @c ustore_status_unsupported_k The process may allocate on no memory domain, several threads
 *    are asked of a build without them, no class this build compiled fits under the
 *    @c cpu_class_ceiling on this CPU, or the operating system denies ForkUnion an operation.
 */
USTORE_EXPORT void ustore_executor_for_ram(ustore_executor_for_ram_t* arguments);

/** How the NVMe engine moves bytes between its files and memory. */
typedef enum ustore_transport_t {

    /** io_uring where the kernel offers it, Linux AIO where it offers only that, and blocking calls elsewhere. */
    ustore_transport_automatic_k = 0,

    /** Positioned reads and writes that block the polling thread, which every platform has. */
    ustore_transport_blocking_k = 1,

    /** io_uring on unbuffered files, refused where the kernel creates no ring. */
    ustore_transport_linux_uring_k = 2,

    /** Linux AIO on unbuffered files. */
    ustore_transport_linux_aio_k = 3,

    /** SPDK's user-space driver on NVMe devices the process owns. */
    ustore_transport_linux_spdk_k = 4,

} ustore_transport_t;

/** The canonical spelling of a transport, as @c ustore_transport_linux_uring_k spells "linux-uring", or
 *  "unrecognized". */
USTORE_EXPORT char const* ustore_transport_name(ustore_transport_t transport);

/** Reads a transport back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_transport_from_name(ustore_bytes_view_t name, ustore_transport_t* transport);

/**
 *  @brief How the threads of an NVMe executor move bytes between the drives and memory, where every zero size or count
 *      keeps the engine's default.
 *
 *  Every thread of the executor, and the call holding it, keeps a reading and a writing channel of
 *  its own, with the media of every database its calls touch registered on it, so no channel is
 *  shared between queue sets. A channel is one queue pair of @c queue_depth submissions over the
 *  thread's pinned request and streaming buffers.
 */
typedef struct ustore_nvme_options_t {

    /** How bytes move between the files and memory. */
    ustore_transport_t transport;

    /** Submissions a channel holds at once; zero fits the disks under @c paths, their tags
     *  shared by the threads on each hardware queue, up to @c request_buffers_count, or 64 when
     *  no disk answers. */
    ustore_size_t queue_depth;

    /** Bytes of each request buffer of a channel. */
    ustore_size_t request_buffer_bytes;

    /** Request buffers of each channel. */
    ustore_size_t request_buffers_count;

    /** Bytes of each streaming buffer of a channel. */
    ustore_size_t stream_buffer_bytes;

    /** Streaming buffers of each channel. */
    ustore_size_t stream_buffers_count;

    /** The data directories, raw media or SPDK namespaces of the databases the executor will open,
     *  spelled as @c ustore_database_open_nvme names them and read only during the call: the memory
     *  domain they share places the threads and their staging unless
     *  @c ustore_executor_for_nvme_t::ram names domains, their fewest hardware queues give the
     *  default thread count, and their block queues give the default depth; @c NULL derives nothing
     *  from any medium. */
    ustore_bytes_view_t const* paths;

    /** Entries of @c paths. */
    ustore_size_t paths_count;

} ustore_nvme_options_t;

/** The arguments of @c ustore_executor_for_nvme. */
typedef struct ustore_executor_for_nvme_t {

    /** Output for the new executor, which the caller frees with @c ustore_executor_free; @c NULL on failure. */
    ustore_executor_t* executor;

    /** Output for the outcome, final on return. */
    ustore_outcome_t* outcome;

    /** How the databases it opens move bytes, and where their media sit; copied at creation, so the
     *  caller may free it once the call returns. */
    ustore_nvme_options_t nvme;

    /** The memory domains its threads and their staging live on, the first of them placing the
     *  threads; with none named, the domain the @c nvme paths share on a machine of several. The
     *  executor reads nothing else of the RAM options. */
    ustore_ram_options_t ram;

    /** Threads to spawn, each owning one submission ring, where zero takes the fewest hardware
     *  queues of any disk under the @c nvme paths, capped by the cores, or half the cores and at
     *  most eight when no disk reports its queues. */
    ustore_size_t threads;

    /** The newest CPU class the threads may confine their waits and atomics to; a class the CPU
     *  lacks narrows the pick rather than refusing it, and zero sets no ceiling. */
    ustore_cpu_class_t cpu_class_ceiling;

    /** The ceiling on each thread's scratch, which every call's staging carves from, past which a
     *  call answers @c ustore_status_out_of_memory_k; zero takes 16 GiB. */
    ustore_size_t scratch_limit;

} ustore_executor_for_nvme_t;

/**
 *  @brief Creates an executor for the NVMe engine, whose every thread drives its own I/O ring.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  A database @c ustore_database_open_nvme opens on it moves bytes as @c nvme names, which the open
 *  reads once, so the executor may be freed before the databases it opened. Where its merges run is
 *  the open's own @c compactions. Its threads pin to the first cores the calling thread may run on,
 *  so a caller making several executors narrows its affinity to each one's cores first, or they all
 *  share the same cores. Every call on an NVMe database names an NVMe executor or none: a RAM
 *  executor answers @c ustore_status_device_k, and a call naming none runs over blocking queues
 *  made for it alone, which an SPDK database refuses with @c ustore_status_invalid_argument_k.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c executor output is @c NULL, the transport is
 *    unknown, @c paths is @c NULL with a nonzero count, a memory domain identifier names no
 *    domain, the ceiling names no CPU class, or ForkUnion refuses an argument.
 *  - @c ustore_status_unsupported_k The process may allocate on no memory domain, several threads
 *    are asked of a build without them, no class this build compiled fits under the
 *    @c cpu_class_ceiling on this CPU, or the operating system denies ForkUnion an operation.
 */
USTORE_EXPORT void ustore_executor_for_nvme(ustore_executor_for_nvme_t* arguments);

/** How a VRAM executor moves bytes between the drives and its device through GPUDirect Storage. */
typedef enum ustore_gds_t {

    /** Direct transfers where the @c nvidia_fs driver is loaded, and cuFile's compatibility mode elsewhere. */
    ustore_gds_automatic_k = 0,

    /** Direct transfers, refused with @c ustore_status_device_k where the driver is absent. */
    ustore_gds_direct_k = 1,

    /** cuFile's compatibility mode, which bounces through host pages it manages. */
    ustore_gds_compatibility_k = 2,

} ustore_gds_t;

/** The canonical spelling of a GPUDirect Storage mode, as @c ustore_gds_compatibility_k spells
 *  "compatibility", or "unrecognized". */
USTORE_EXPORT char const* ustore_gds_name(ustore_gds_t gds);

/** Reads a GPUDirect Storage mode back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_gds_from_name(ustore_bytes_view_t name, ustore_gds_t* gds);

/** The GPU vendors one binary drives at once, each through a runtime opened on first use. */
typedef enum ustore_gpu_vendor_t {

    /** No vendor, which host pages and a device no driver reports hold. */
    ustore_gpu_vendor_unknown_k = 0,

    /** NVIDIA, through the CUDA driver when one is installed. */
    ustore_gpu_vendor_nvidia_k = 1,

    /** AMD, through the HIP runtime when one is installed. */
    ustore_gpu_vendor_amd_k = 2,

    /** Apple, through Metal, present on every Apple silicon Mac. */
    ustore_gpu_vendor_apple_k = 3,

} ustore_gpu_vendor_t;

/** The canonical spelling of a GPU vendor, as @c ustore_gpu_vendor_amd_k spells "amd", or "unrecognized". */
USTORE_EXPORT char const* ustore_gpu_vendor_name(ustore_gpu_vendor_t vendor);

/** Reads a GPU vendor back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_gpu_vendor_from_name(ustore_bytes_view_t name, ustore_gpu_vendor_t* vendor);

/** The GPU generations UStore tells apart, the device twin of @c ustore_cpu_class_t: a class implies its vendor, and
 *  a device takes its vendor's newest class whose code it runs. */
typedef enum ustore_gpu_class_t {

    /** No class, which a listed device holds when this build compiled no code that runs on it. */
    ustore_gpu_class_unknown_k = 0,

    // NVIDIA

    /** Ampere-class NVIDIA - A100, A10 & RTX 30, and every later GPU that compiles its PTX. */
    ustore_gpu_class_nvidia_ampere_k = 1,

    /** Ada-class NVIDIA - L4, L40S & RTX 40. */
    ustore_gpu_class_nvidia_ada_k = 2,

    /** Hopper-class NVIDIA - H100 & H200. */
    ustore_gpu_class_nvidia_hopper_k = 3,

    /** Blackwell-class NVIDIA - B200, B300 & GB300. */
    ustore_gpu_class_nvidia_blackwell_k = 4,

    /** Consumer Blackwell NVIDIA - RTX 50, RTX PRO 6000 & DGX Spark. */
    ustore_gpu_class_nvidia_blackwellrtx_k = 5,

    // AMD

    /** CDNA3 AMD - MI300X & MI325X, gfx942. */
    ustore_gpu_class_amd_cdna3_k = 7,

    /** CDNA4 AMD - MI350X & MI355X, gfx950. */
    ustore_gpu_class_amd_cdna4_k = 8,

    /** CDNA5 AMD - MI400, gfx1250. */
    ustore_gpu_class_amd_cdna5_k = 9,

    // Apple

    /** Apple7-family GPUs and every Apple GPU without a class of its own - M1 & M2. */
    ustore_gpu_class_apple_metal7_k = 10,

    /** Apple9-family GPUs - M3 & M4. */
    ustore_gpu_class_apple_metal9_k = 11,

    /** Apple10-family GPUs - M5. */
    ustore_gpu_class_apple_metal10_k = 12,

} ustore_gpu_class_t;

/** The canonical spelling of a GPU class, as @c ustore_gpu_class_amd_cdna3_k spells "amd-cdna3", or "unrecognized". */
USTORE_EXPORT char const* ustore_gpu_class_name(ustore_gpu_class_t gpu_class);

/** Reads a GPU class back from its canonical spelling, or fails with
 *  @c ustore_status_invalid_argument_k. */
USTORE_EXPORT ustore_status_t ustore_gpu_class_from_name(ustore_bytes_view_t name, ustore_gpu_class_t* gpu_class);

/** One GPU as UStore numbers it: its position in the listing of @c ustore_gpu_devices is the ordinal
 *  @c ustore_vram_options_t::devices names it by. */
typedef struct ustore_gpu_device_t {

    /** The vendor whose runtime drives the device. */
    ustore_gpu_vendor_t vendor;

    /** The device's ordinal in its vendor's runtime, as @c nvidia-smi or @c rocm-smi list it. */
    uint32_t vendor_ordinal;

    /** The class of the device. */
    ustore_gpu_class_t gpu_class;

    /** Streaming multiprocessors, compute units or GPU cores. */
    uint32_t compute_units;

    /** Device memory in bytes, or the working set Metal recommends for unified memory. */
    uint64_t memory_bytes;

    /** The vendor's own name for the device, null-terminated and truncated to fit. */
    char name[64];

} ustore_gpu_device_t;

/** The arguments of @c ustore_gpu_devices. */
typedef struct ustore_gpu_devices_t {

    /** Output for the outcome, final on return. */
    ustore_outcome_t* outcome;

    /** Output for up to @c devices_capacity devices, in UStore ordinal order; may be @c NULL. */
    ustore_gpu_device_t* devices;

    /** Entries @c devices holds. */
    ustore_size_t devices_capacity;

    /** The @b optional output for every device this process can drive, which may exceed @c devices_capacity. */
    ustore_size_t* devices_count;

} ustore_gpu_devices_t;

/**
 *  @brief Lists every GPU this process can drive, NVIDIA devices first, then AMD, then Apple, so
 *      the position of a device is the ordinal @c ustore_vram_options_t::devices names it by.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  Each vendor's runtime opens on the first call, and a vendor whose runtime is absent lists no
 *  devices, so the call succeeds with a zero count on a machine without GPUs. Each device lists the
 *  newest class this build compiled code for that runs on it, or @c ustore_gpu_class_unknown_k
 *  where none does. The listing is taken once per process and never changes after.
 */
USTORE_EXPORT void ustore_gpu_devices(ustore_gpu_devices_t* arguments);

/** The GPUs a VRAM executor drives, one stream each, all of one vendor. */
typedef struct ustore_vram_options_t {

    /** The UStore ordinals of @c ustore_gpu_devices the executor drives, copied by the call,
     *  where a repeated ordinal is driven once; @c NULL takes every listed device of the first
     *  vendor with a known class, each of a known class itself. */
    int32_t const* devices;

    /** Entries of @c devices, where zero derives as @c NULL does. */
    ustore_size_t devices_count;

    /** The @b optional newest class dispatch may pick: every device runs the newest class all of
     *  them load at or under it, so an H100 beside an L4 runs Ada code; a ceiling under which some
     *  device loads none is refused, and zero sets no ceiling. */
    ustore_gpu_class_t gpu_class_ceiling;

} ustore_vram_options_t;

/** The arguments of @c ustore_executor_for_vram. */
typedef struct ustore_executor_for_vram_t {

    /** Output for the new executor, which the caller frees with @c ustore_executor_free; @c NULL on failure. */
    ustore_executor_t* executor;

    /** Output for the outcome, final on return. */
    ustore_outcome_t* outcome;

    /** The devices the executor drives, read only during the call. */
    ustore_vram_options_t vram;

    /** The @b optional streams to adopt, one per entry of @c vram.devices and on that device, an
     *  @c id<MTLCommandQueue> on Apple. On success a CUDA or HIP stream becomes the executor's,
     *  which synchronizes and destroys it in @c ustore_executor_free; a Metal queue is retained,
     *  the executor releasing its own reference there while the caller keeps and releases theirs.
     *  On failure every stream stays the caller's as it was. @c NULL has the executor open its
     *  own; default streams are refused. */
    void* const* streams;

    /** The @b optional ceiling on the device scratch a call or a merge takes, per device, so N
     *  devices hold up to N times it; zero lets scratch grow as calls ask. */
    ustore_size_t scratch_limit;

    /** How reads and compactions move bytes between the drives and the devices. */
    ustore_gds_t gds;

} ustore_executor_for_vram_t;

/**
 *  @brief Orders work on one stream per device of @c vram, opened or adopted with the executor and
 *      destroyed with it, every device fed by the one calling thread.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  The executor spawns no pool: its caller is its one host thread, reading drives through blocking
 *  queues the executor owns, so a warm call allocates nothing. Every call queues from the calling
 *  thread, which enters each device in turn and waits on it there, so a deferred call is final when
 *  it returns, as a blocking one is, and no thread is pinned to a device. Arenas the executor makes
 *  live on its first device, where every call computes, except a filtered scan, which deals its
 *  pages across every device and narrows each on the host. Its calls read through an engine's
 *  device verbs, which refuse changes, the control plane and every call no device tower serves yet
 *  with @c ustore_status_unsupported_k. @c ustore_database_open_nvme takes one as its
 *  @c compactions, reading its first device and scratch limit once. Every build makes one, as the
 *  vendor's runtime opens at the first call and the library links none.
 *
 *  A CUDA stream must sit on its device's primary context, which the runtime API creates streams
 *  on. A stream under CUDA graph capture can't be waited on from the host, so a call on it fails
 *  with @c ustore_status_device_k. Each call leaves the primary context of the last device it drove
 *  current on the calling thread, as @c cuCtxSetCurrent does.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c executor output is @c NULL, the @c gds mode or the
 *    @c gpu_class_ceiling is unknown, ForkUnion refuses an argument, a device named twice beside
 *    @c streams, @c streams beside no named device at all, or a null or default stream, which no
 *    executor can destroy.
 *  - @c ustore_status_config_mismatch_k Devices of more than one vendor, or a stream on another
 *    device than its entry names.
 *  - @c ustore_status_unsupported_k No class this build compiled serves every device at or under
 *    the @c gpu_class_ceiling, the process may allocate on no memory domain, or the operating
 *    system denies ForkUnion an operation.
 *  - @c ustore_status_device_k No device holds an ordinal, none has a known class, a driver
 *    refuses a stream or an event, or @c ustore_gds_direct_k names a device other than NVIDIA.
 */
USTORE_EXPORT void ustore_executor_for_vram(ustore_executor_for_vram_t* arguments);

/** The arguments of @c ustore_executor_drain. */
typedef struct ustore_executor_drain_t {

    /** The executor whose call is awaited. */
    ustore_executor_t executor;

    /** Output for an executor-level failure, such as a device fault, never a drained call's. */
    ustore_outcome_t* outcome;

    /** What the executor's threads do until its next call. */
    ustore_mood_t mood;

} ustore_executor_drain_t;

/**
 *  @brief Waits until no call holds the executor, then applies the mood, reporting only
 *      executor-level failures, and contention from a callback of the call it would wait on.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c mood is unknown.
 *  - @c ustore_status_contended_k A callback on the thread serving the holding call drains it.
 */
USTORE_EXPORT void ustore_executor_drain(ustore_executor_drain_t* arguments);

/**
 *  @brief Waits until no call holds the executor and its device ran the work queued on its stream,
 *      releases its scratch and frees it; @c NULL is safe, and it cannot fail, so takes no outcome.
 *  @param[in,out] executor The executor to be freed; invalid afterwards.
 *
 *  Every arena it made must be freed first.
 */
USTORE_EXPORT void ustore_executor_free(ustore_executor_t executor);

/**
 *  @brief Memory the library writes every output whose length scales with a call into, reused
 *      across calls. @ref types_memory
 *
 *  Made by @c ustore_arena_init where its executor computes, and an executor whose device runs
 *  out of memory reports @c ustore_status_out_of_memory_k rather than falling back to the host.
 *  Each call given the arena rewinds it, which invalidates every output of the previous call,
 *  and refills it, growing only when the new outputs outsize everything it held, so a steady
 *  loop allocates once. Every output starts aligned to 64 bytes, as Arrow buffers do, so typed
 *  columns read in place. To keep one call's outputs alive past the next call, pass the next
 *  call another arena.
 *
 *  A @c NULL arena is refused with @c ustore_status_invalid_argument_k, and so is a call whose
 *  inputs point into the arena it is about to rewind. An arena serves one call at a time, so a
 *  second call naming it finishes @c ustore_status_contended_k, and a deferred one does so at
 *  submission. A call whose executor computes on host threads given an arena on a device, or on
 *  another device than the arena's, reports @c ustore_status_device_k. A blocking call uses the
 *  arena until it returns, and a deferred one until final. A call that fails, is cancelled, or has
 *  nothing to report leaves its outputs @c NULL.
 */
typedef struct ustore_arena_handle_t* ustore_arena_t;

/** The arguments of @c ustore_arena_init. */
typedef struct ustore_arena_init_t {

    /** Output for the new arena, which the caller frees with @c ustore_arena_free; @c NULL on failure. */
    ustore_arena_t* arena;

    /** The executor whose calls the arena serves, which decides where it lives, as a RAM executor places it on the
     *  first of its memory domains; @c NULL serves calls on the calling thread from host pages. */
    ustore_executor_t executor;

    /** Output for the outcome, final on return. */
    ustore_outcome_t* outcome;

} ustore_arena_init_t;

/**
 *  @brief Creates an empty arena where @c executor computes, so its placement is decided once and device code keeps
 *      its outputs on the device without asking.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  Nothing is allocated until a call writes into it. An arena must not outlive the executor that
 *  made it: free every arena before its executor. An arena made with a @c NULL executor has none,
 *  and lives until it is freed.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c arena output is @c NULL.
 */
USTORE_EXPORT void ustore_arena_init(ustore_arena_init_t* arguments);

/**
 *  @brief Frees the arena and every output in it, once every deferred call naming it is final;
 *      @c NULL is safe.
 *  @param[in,out] arena The arena to be freed; invalid afterwards, unless the free is refused.
 *
 *  @par Statuses:
 *  - @c ustore_status_contended_k An export of the arena's outputs is open, and nothing is freed.
 */
USTORE_EXPORT ustore_status_t ustore_arena_free(ustore_arena_t arena);

/**
 *  @brief Opens one export of @p arena's outputs, a view a consumer reads in place: until the
 *      matching @c ustore_arena_export_close, every call naming the arena answers
 *      @c ustore_status_contended_k before it claims its executor, and the arena's free refuses.
 *  @param[in,out] arena The arena whose outputs a view hands out.
 *
 *  A binding opens one export per view it hands to a consumer and closes it from that view's
 *  deleter or release callback; describing the arena answers as usual meanwhile.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @p arena is @c NULL.
 *  - @c ustore_status_contended_k A call holds the arena.
 *  - @c ustore_status_overflow_k The arena holds as many open exports as 32 bits count.
 */
USTORE_EXPORT ustore_status_t ustore_arena_export_open(ustore_arena_t arena);

/** Closes one export @c ustore_arena_export_open opened on @p arena, from any thread and in any
 *  order; @c NULL or an arena with no open export is left as it is. */
USTORE_EXPORT void ustore_arena_export_close(ustore_arena_t arena);

/** Where an arena's outputs live, as @c ustore_arena_describe reports it. */
typedef enum ustore_residency_t {

    /** No residency, which only a zeroed description reads as. */
    ustore_residency_unknown_k = 0,

    /** Host pages, which the calling thread reads in place. */
    ustore_residency_ram_k = 1,

    /** One device's memory, which only that device's code reads. */
    ustore_residency_vram_k = 2,

} ustore_residency_t;

/** Whether host code may read an arena's pages in place. */
typedef enum ustore_host_access_t {

    /** Host code must not read the pages, which only their device's code reaches. */
    ustore_host_access_refused_k = 0,

    /** Host code reads the pages in place: host, pinned, managed or Apple's shared memory. */
    ustore_host_access_readable_k = 1,

} ustore_host_access_t;

/** The canonical spelling of a host access, as @c ustore_host_access_readable_k spells "readable",
 *  or "unrecognized". */
USTORE_EXPORT char const* ustore_host_access_name(ustore_host_access_t host_access);

/**
 *  @brief Where an arena's outputs live, and whether host code may read them.
 *
 *  The device type an export names follows from these fields alone. VRAM that host code may not
 *  read is the device's own memory, and VRAM it may read is managed memory on NVIDIA and shared
 *  memory on Apple. RAM with a @c device of zero or more is host memory page-locked for that
 *  device, and RAM with a @c device of -1 is plain host memory.
 */
typedef struct ustore_arena_description_t {

    /** RAM for host pages, VRAM for one device's memory. */
    ustore_residency_t residency;

    /** The UStore ordinal of the device as @c ustore_gpu_devices lists it, or -1 for host pages. */
    int32_t device;

    /** The vendor of that device as the listing reports it, or unknown for host pages. */
    ustore_gpu_vendor_t vendor;

    /** The vendor's own number of the device, as Arrow and DLPack name it, or -1 for host pages. */
    int32_t vendor_ordinal;

    /** Whether host code may read the pages in place. */
    ustore_host_access_t host_access;
} ustore_arena_description_t;

/** The arguments of @c ustore_arena_describe. */
typedef struct ustore_arena_describe_t {

    /** The arena to describe. */
    ustore_arena_t arena;

    /** Output for the outcome, final on return. */
    ustore_outcome_t* outcome;

    /** Output for where the arena's outputs live. */
    ustore_arena_description_t* description;
} ustore_arena_describe_t;

/**
 *  @brief Reports where an arena's outputs live, as the executor that made it decided, so a caller
 *      reads them as host memory or hands them to device code without remembering that executor.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k A @c NULL arena or description.
 */
USTORE_EXPORT void ustore_arena_describe(ustore_arena_describe_t* arguments);

/**
 *  @brief Reports the version of the library the program runs against, which may differ from the macros it
 *      compiled against.
 *
 *  None of the three outputs may be @c NULL.
 *
 *  @param[out] major Output for the major version.
 *  @param[out] minor Output for the minor version.
 *  @param[out] patch Output for the patch version.
 */
USTORE_EXPORT void ustore_version(int* major, int* minor, int* patch);

/** The version of the library the program runs against as a static string, pre-release suffix
 *  included, like "1.0.0-rc.1". */
USTORE_EXPORT char const* ustore_version_name(void);

#ifdef __cplusplus
}
#endif

#endif // UNUM_USTORE_TYPES_H
