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
 *  Every verb that takes options, outputs, or callbacks takes one argument structure named after it, as
 *  @c ustore_blobs_read takes a @c ustore_blobs_read_t, so optional arguments are skipped by name and their order
 *  never matters. Name lookups, views, outcome polls and the @c _free family stay positional. Every structure
 *  opens in the same order: the handle, @c executor, @c outcome, then @c completion. The rest reads in the
 *  order the call works: what it reads, led by the view of a read or the transaction of a write, then the
 *  outputs and the @c written count of a listing right before the @c arena they land in, then the knobs that
 *  shape the work, such as @c wanted, a conversion of the outputs such as @c outputs_type, and @c mode, and last
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
 *  argument structure is the whole request and the outcome is all the caller observes until it
 *  finishes, which lets a thread pool, a GPU stream, or a remote client serving the same header over the
 *  network sit behind the same verbs without changing a call site. Every call names its completion: a
 *  blocking call, the default, returns with its final status stored and every output
 *  readable, and a call whose @c completion is @c ustore_completion_deferred_k returns once submitted, as
 *  an io_uring entry does. A call with a @c NULL executor runs on the calling thread and is final when
 *  it returns, whatever its @c completion. Submission copies the structure; everything it points to
 *  stays borrowed until final.
 *
 *  The outcome's status reads @c ustore_status_deferred_k until final, even for a call refused at
 *  submission. The message, then the final status, are stored with release ordering and a wake, and the
 *  library touches nothing the structure points to after that store. The caller reads the status with
 *  acquire ordering, polling through @c ustore_outcome_status or waiting through @c ustore_outcome_wait.
 *  Device work stores the final status from a host callback that still fires after a device fault, so an
 *  outcome may live in any host memory.
 *
 *  Scheduling is bounded on both sides. Calls on one executor start in submission order, and a blocking
 *  call runs alone: after every call submitted before it finishes, and before any submitted after it
 *  starts. Executors own their threads and streams, so verbs take no stream and no event, and a caller
 *  orders dependent deferred calls itself, by waiting on the first before submitting the second. Each
 *  executor holds a fixed number of deferred calls in its deferred table, sized by
 *  @c deferred_limit, so a deferred submission that finds the table full finishes at once with
 *  @c ustore_status_contended_k instead of allocating or blocking, while blocking calls take no slot. An
 *  arena and a transaction each serve one call at a time, and a second call naming one finishes
 *  @c ustore_status_contended_k. A call stops early only when its progress
 *  callback returns @c ustore_cancel_k, so @c ustore_status_deferred_k is the only status an unfinished
 *  call shows.
 *
 *  @code{.c}
 *  ustore_outcome_t outcomes[4] = {0};
 *  for (size_t scan = 0; scan != 4; ++scan) {
 *      scans[scan].executor = executor, scans[scan].outcome = &outcomes[scan], scans[scan].arena = arenas[scan];
 *      scans[scan].completion = ustore_completion_deferred_k;
 *      ustore_blobs_scan(&scans[scan]);
 *  }
 *  for (size_t scan = 0; scan != 4; ++scan) ustore_outcome_wait(&outcomes[scan]);
 *  @endcode
 *
 *  @section types_memory Memory
 *
 *  Two complications shape memory, and nothing else may: outputs whose size only the backend
 *  knows, and memory a GPU should place itself. Arenas answer the first, executors the second, and
 *  every binding stays one verb per operation rather than a twin per output style. Every byte has
 *  one owner, and each owner picks where its bytes live once, when it is created.
 *
 *  Callers lend memory, never a way to allocate it: no allocator callback crosses the interface, and
 *  every pointer a caller passes is an input, classified through the driver once per call. Payload
 *  belongs to the handle, placed by its constructor, as @c ustore_database_open_nvme
 *  places it on its drives. Scratch and staging belong to the executor, placed at its creation and
 *  grown to a high-water mark. Every output whose length scales with the call lands in an arena:
 *  values, presences, offsets, keys, identifiers, degrees, edges, bounds and columns alike. Only
 *  fixed-size reports, such as descriptions, stamps, the @c written count of a listing and created handles,
 *  stay caller fields, and a call that fails zeroes the scalars among them and leaves the handles @c NULL.
 *
 *  Executors make arenas. @c ustore_arena_init places one where its executor computes, decided once,
 *  and a @c NULL executor makes host pages for calls on the calling thread. A call rewinds the arena it
 *  is given, invalidating the previous call's outputs, and writes views into it, and any verb may reuse
 *  any arena its executor made, one call at a time. Blocking calls borrow the arena; deferred calls hold
 *  it until final. Shorthand with no place for an arena, such as @c db[key] or @c key @c in @c db, makes a
 *  one-off arena through @c ustore_arena_init and frees it with its result: slower than reuse, and
 *  stateless.
 *
 *  Outputs are views the call writes, so none carries a capacity or a stride for the caller to
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
 *  A verb's @c count is its batch axis, so a single operation is a batch of one. Every input takes one of three
 *  shapes. One run of bytes, a @c ustore_bytes_view_t, carries a path, a name or a request. A strided run of
 *  fixed-width elements, such as a @c ustore_keys_view_t, carries one element per task with a byte stride, as BLAS
 *  does, so callers that pack arguments into arrays of structures pass them without copies, and a zero stride
 *  repeats the first element for every task. A ragged batch, a @c ustore_strings_view_t, carries one string per task
 *  as an Apache Arrow tape of 32-bit or 64-bit offsets, or through an accessor over any other shape, so a tape read
 *  back is written again as it is. A zeroed view of any shape stands for the input's default.
 *
 *  Outputs take three shapes too, written packed into the arena, so none carries a capacity or a stride. Fixed-width
 *  outputs are arrays, such as presence bits, keys, identifiers, degrees and bounds. Ragged bytes are a
 *  @c ustore_strings_view_t the call writes as a tape, with 32-bit offsets while its bytes fit in 4 GiB and 64-bit
 *  beyond. Ragged keys are packed, split by task through @c count + 1 offsets, as every offset array holds one more
 *  entry than the runs it splits.
 */

#ifndef UNUM_USTORE_TYPES_H
#define UNUM_USTORE_TYPES_H

#include <stddef.h> // `size_t`
#include <stdint.h> // `uint8_t`, `uint32_t`, `uint64_t`

/**
 *  @brief Marks every function the library exports, and nothing else.
 *
 *  On Windows it exports from the shared library, which builds with @c USTORE_EXPORTING, imports where a consumer of
 *  that library defines @c USTORE_SHARED, as linking @c ustore::shared does, and expands to nothing for the static
 *  library. GCC and Clang give the function default visibility, so it survives @c -fvisibility=hidden.
 */
#if !defined(USTORE_EXPORT)
#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(USTORE_EXPORTING)
#define USTORE_EXPORT __declspec(dllexport)
#elif defined(USTORE_SHARED)
#define USTORE_EXPORT __declspec(dllimport)
#else
#define USTORE_EXPORT
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define USTORE_EXPORT __attribute__((visibility("default")))
#else
#define USTORE_EXPORT
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

/** Writes the view of every string <tt>indices[i]</tt> into <tt>views[i]</tt>, for strings no tape holds, and accepts
 *  any
 *  @p count. */
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

/** What went wrong, in one vocabulary shared by every engine and binding. Shared statuses carry USearch's numbers,
 *  leaving 3, 4, 5, 11 and 13 unused, and UStore's own follow from 18.
 *
 *  Statuses fall into five categories. Resources: @c ustore_status_out_of_memory_k, @c ustore_status_io_k and
 *  @c ustore_status_device_k. Input: @c ustore_status_invalid_argument_k and @c ustore_status_malformed_k. State:
 *  @c ustore_status_uninitialized_k, @c ustore_status_read_only_k, @c ustore_status_unsupported_k,
 *  @c ustore_status_not_found_k, @c ustore_status_already_exists_k, @c ustore_status_write_conflict_k,
 *  @c ustore_status_read_conflict_k, @c ustore_status_phantom_conflict_k, @c ustore_status_capacity_exhausted_k and
 *  @c ustore_status_contended_k. Interruptions: @c ustore_status_cancelled_k and @c ustore_status_deferred_k.
 *  Defects: @c ustore_status_internal_k. Any verb that allocates, binds a device or touches a file may also return a
 *  Resources status; any verb taking a progress callback may return @c ustore_status_cancelled_k. A verb's own block
 *  lists only what its contract adds. */
typedef enum ustore_status_t {

    /** The call succeeded; @c message carries nothing. */
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

    /** Stored or supplied data could not be decoded, like a corrupted file or an invalid document. */
    ustore_status_malformed_k = 9,

    /** The engine failed in a way no other status names, which points at a defect. */
    ustore_status_internal_k = 10,

    /** The call needs a database or transaction that was never opened. */
    ustore_status_uninitialized_k = 12,

    /** The call was refused rather than waited for: its executor's deferred table is full, its arena or its
     *  transaction serves another call, or it drains or blocks from a callback its own executor runs. */
    ustore_status_contended_k = 14,

    /** A progress callback stopped the call, which unwound and published nothing. */
    ustore_status_cancelled_k = 15,

    /** The engine or build does not implement what the call asks for, like io_uring on a kernel without it. */
    ustore_status_unsupported_k = 16,

    /** An argument is null, contradicts another one, or carries a choice the call does not accept, like a collection
     *  of another modality, a name reserved for a system collection, or a batch of strings in an unknown layout. */
    ustore_status_invalid_argument_k = 17,

    /** A collection, a directory, or a document a modification needs does not exist, while a read marks a missing
     *  key with an unset presence bit instead. */
    ustore_status_not_found_k = 18,

    /** A collection name or a document the call creates is already taken. */
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

/** Outcome of one call: a status to branch on, and a static message that is never freed and is @c NULL exactly
 *  when the status is @c ustore_status_success_k. */
typedef struct ustore_outcome_t {

    /** What went wrong, @c ustore_status_success_k, or @c ustore_status_deferred_k until a deferred call finishes. */
    ustore_status_t status;

    /** Static detail the status cannot carry. */
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
 *  a value no status carries. */
USTORE_EXPORT char const* ustore_status_name(ustore_status_t status);

/**
 *  Reads a status back from its canonical spelling.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k No status carries the @p name.
 */
USTORE_EXPORT ustore_status_t ustore_status_from_name(ustore_bytes_view_t name);

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
 *  Counts are in the units the verb consumes: tasks for a scan, one pull for a cursor, bytes of keys and values for a
 *  save, and bytes or collections for a compaction. The callback never runs concurrently with itself, however many
 *  threads do the work, and never after the call stores its final status. Its arguments are the units finished so
 *  far, never decreasing within one call, the units the call finishes if it runs to the end, and the
 *  @c progress_state registered beside it.
 */
typedef ustore_continuation_t (*ustore_progress_callback_t)(ustore_size_t finished, ustore_size_t total, void* state);

/** What the strings of a @c ustore_patterns_t are matched as. */
typedef enum ustore_pattern_kind_t {

    /** Regular expressions, of which a listed value matches at least one somewhere. */
    ustore_pattern_regex_k = 0,

    /** Literal substrings, of which a listed value holds at least one, found together through Aho-Corasick. */
    ustore_pattern_substrings_k = 1,

} ustore_pattern_kind_t;

/** The canonical spelling of a pattern kind, as @c ustore_pattern_regex_k spells "regex", or "unrecognized". */
USTORE_EXPORT char const* ustore_pattern_kind_name(ustore_pattern_kind_t kind);

/** Reads a pattern kind back from its canonical spelling; @c ustore_pattern_regex_k when unrecognized. */
USTORE_EXPORT ustore_pattern_kind_t ustore_pattern_kind_from_name(ustore_bytes_view_t name);

/** Whether patterns compare bytes as they are or after Unicode case folding. */
typedef enum ustore_case_sensitivity_t {

    /** Bytes as they are. */
    ustore_cased_k = 0,

    /** Full Unicode case folding of both the patterns and the values. */
    ustore_uncased_k = 1,

} ustore_case_sensitivity_t;

/** The canonical spelling of a case sensitivity, as @c ustore_uncased_k spells "uncased", or "unrecognized". */
USTORE_EXPORT char const* ustore_case_sensitivity_name(ustore_case_sensitivity_t sensitivity);

/** Reads a case sensitivity back from its canonical spelling; @c ustore_cased_k when unrecognized. */
USTORE_EXPORT ustore_case_sensitivity_t ustore_case_sensitivity_from_name(ustore_bytes_view_t name);

/**
 *  @brief Patterns a scan keeps the values or strings matching, shared by every task of the call and compiled once per
 *      call or once per cursor.
 *
 *  A zeroed structure matches everything. Patterns that fail to compile refuse the call with
 *  @c ustore_status_invalid_argument_k, carrying StringZilla's message.
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
 *  Its kind picks where calls compute: host threads for every engine, or one device for the reads and scans an engine
 *  computes there, where a change, the control plane and every call no device tower serves yet answer
 *  @c ustore_status_unsupported_k, and an arena or cursor of another kind of executor @c ustore_status_device_k.
 *  Calls on one executor start in submission order, and a blocking call runs alone: after every call submitted
 *  before it finishes, and before any submitted after it starts. Databases stay freely concurrent. A blocking call
 *  runs on the calling thread, while one driver thread hands deferred calls in batches to the executor's threads.
 *  In a build without OS threads, an executor runs every call at submission, before returning. It owns the scratch
 *  and staging its calls need, placed where it computes and grown to a high-water mark, so a steady workload
 *  allocates once: a RAM executor keeps the scratch of each thread on the memory domain local to it.
 *
 *  An executor holds its deferred calls in a deferred table sized at creation by @c deferred_limit, so submitting
 *  never allocates, and a deferred submission that finds the table full stores @c ustore_status_contended_k as its
 *  final status. A call leaves the table before its status turns final, so the table is full only of calls no
 *  caller has seen final. From inside a progress callback its own executor runs, @c ustore_executor_drain and a
 *  blocking call on that executor report @c ustore_status_contended_k, so nothing waits on itself. Frees cannot
 *  report, so @c ustore_executor_free is never called there, nor the free of a handle a deferred call names.
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

/** What an executor's threads or rings do once it goes idle. */
typedef enum ustore_mood_t {

    /** Parked, so the next call pays a wake-up. */
    ustore_mood_parked_k = 0,

    /** Spinning, trading power for the next call's latency. */
    ustore_mood_spinning_k = 1,

} ustore_mood_t;

/** The canonical spelling of a mood, as @c ustore_mood_spinning_k spells "spinning", or "unrecognized". */
USTORE_EXPORT char const* ustore_mood_name(ustore_mood_t mood);

/** Reads a mood back from its canonical spelling; @c ustore_mood_parked_k when unrecognized. */
USTORE_EXPORT ustore_mood_t ustore_mood_from_name(ustore_bytes_view_t name);

/**
 *  @brief The host memory domains a RAM executor serves, NUMA nodes as the operating system numbers them.
 *
 *  Names where the executor keeps memory and runs its threads, never where a database keeps its payload. The scratch
 *  of each thread lives on the domain local to that thread when the options name it, and on the first domain named
 *  otherwise, as does every arena the executor makes.
 */
typedef struct ustore_ram_options_t {

    /** Identifiers of the memory domains, copied by the call, where @c NULL derives every domain the process may
     *  allocate on, and an identifier no domain carries is refused with @c ustore_status_invalid_argument_k. */
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

    /** The memory domains the executor serves; copied, not borrowed. */
    ustore_ram_options_t ram;

    /** Threads to spawn, interleaved across the compute domains local to @c ram, where zero takes 7/8 of the cores
     *  those domains carry, rounded down and at least one. */
    ustore_size_t threads;

    /** Deferred calls the executor holds at once, sizing its deferred table, where zero takes a default. */
    ustore_size_t deferred_limit;

} ustore_executor_for_ram_t;

/**
 *  @brief Creates an executor of host threads for the RAM, RocksDB and LevelDB engines.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  The default thread count leaves an eighth of the cores to the driver thread, to blocking calls, which run on their
 *  callers' threads, and to the background flushes and compactions of RocksDB and LevelDB. The threads interleave
 *  across the compute domains local to the memory domains named, where ForkUnion spawns near them: every domain the
 *  process may allocate on, or one alone. Naming several but not all spreads them over the whole machine.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c executor output is @c NULL, a memory domain
 *    identifier names no domain, or ForkUnion refuses an argument.
 *  - @c ustore_status_unsupported_k The process may allocate on no memory domain, several threads
 *    are asked of a build without them, or the operating system denies ForkUnion an operation.
 *  - @c ustore_status_capacity_exhausted_k The @c deferred_limit is past 2^30.
 */
USTORE_EXPORT void ustore_executor_for_ram(ustore_executor_for_ram_t* arguments);

/** The arguments of @c ustore_executor_on_pool. */
typedef struct ustore_executor_on_pool_t {

    /** Output for the new executor, which the caller frees with @c ustore_executor_free; @c NULL on failure. */
    ustore_executor_t* executor;

    /** Output for the outcome, final on return. */
    ustore_outcome_t* outcome;

    /** The memory domains the executor serves, keeping the scratch of each thread and its arenas there; copied, not
     *  borrowed. */
    ustore_ram_options_t ram;

    /** The ForkUnion @c fu_pool_t to borrow; it must outlive the executor, and keeps its lender's mood. */
    void* pool;

    /** Deferred calls the executor holds at once, sizing its deferred table, where zero takes a default. */
    ustore_size_t deferred_limit;

} ustore_executor_on_pool_t;

/**
 *  @brief Creates an executor for the RAM, RocksDB and LevelDB engines over a ForkUnion pool it borrows, so several
 *      libraries in one process share one set of threads.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  A dispatch on a pool that runs another is refused, so a batch finding the pool running its lender's dispatch runs
 *  on the executor's own driver thread instead of forking over the lender.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c executor output is @c NULL, a memory domain
 *    identifier names no domain, or ForkUnion refuses the @c pool or another argument.
 *  - @c ustore_status_unsupported_k The process may allocate on no memory domain, or the operating
 *    system denies ForkUnion an operation.
 *  - @c ustore_status_capacity_exhausted_k The @c deferred_limit is past 2^30.
 */
USTORE_EXPORT void ustore_executor_on_pool(ustore_executor_on_pool_t* arguments);

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

/** Reads a transport back from its canonical spelling; @c ustore_transport_automatic_k when unrecognized. */
USTORE_EXPORT ustore_transport_t ustore_transport_from_name(ustore_bytes_view_t name);

/**
 *  @brief How the threads of an NVMe executor move bytes between the drives and memory, where every zero size or count
 *      keeps the engine's default.
 *
 *  Every thread of the executor keeps a channel of its own, and the threads outside it share @c shared_io_channels
 *  more. A channel is one queue pair of @c queue_depth submissions over pinned request and streaming buffers.
 */
typedef struct ustore_nvme_options_t {

    /** How bytes move between the files and memory. */
    ustore_transport_t transport;

    /** I/O channels the threads outside the executor share. */
    ustore_size_t shared_io_channels;

    /** Submissions a channel holds at once. */
    ustore_size_t queue_depth;

    /** Bytes of each request buffer of a channel. */
    ustore_size_t request_buffer_bytes;

    /** Request buffers of each channel. */
    ustore_size_t request_buffers_count;

    /** Bytes of each streaming buffer of a channel. */
    ustore_size_t stream_buffer_bytes;

    /** Streaming buffers of each channel. */
    ustore_size_t stream_buffers_count;

} ustore_nvme_options_t;

/** The arguments of @c ustore_executor_for_nvme. */
typedef struct ustore_executor_for_nvme_t {

    /** Output for the new executor, which the caller frees with @c ustore_executor_free; @c NULL on failure. */
    ustore_executor_t* executor;

    /** Output for the outcome, final on return. */
    ustore_outcome_t* outcome;

    /** How the databases it opens move bytes; copied, not borrowed. */
    ustore_nvme_options_t nvme;

    /** Threads to spawn, each owning one submission ring, where zero derives them from the cores. */
    ustore_size_t threads;

    /** Deferred calls the executor holds at once, sizing its deferred table, where zero takes a default. */
    ustore_size_t deferred_limit;

} ustore_executor_for_nvme_t;

/**
 *  @brief Creates an executor for the NVMe engine, whose every thread submits and polls its own I/O ring.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  A database @c ustore_database_open_nvme opens on it moves bytes as @c nvme names, which the open reads once, so the
 *  executor may be freed before the databases it opened. Where its merges run is the open's own @c compactions.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c executor output is @c NULL, the transport is
 *    unknown, or ForkUnion refuses an argument.
 *  - @c ustore_status_unsupported_k The process may allocate on no memory domain, several threads
 *    are asked of a build without them, or the operating system denies ForkUnion an operation.
 *  - @c ustore_status_capacity_exhausted_k The @c deferred_limit is past 2^30.
 */
USTORE_EXPORT void ustore_executor_for_nvme(ustore_executor_for_nvme_t* arguments);

/** How a VRAM executor moves bytes between the drives and its device through GPUDirect Storage. */
typedef enum ustore_gds_t {

    /** Direct transfers where the @c nvidia_fs driver is loaded, and cuFile's compatibility mode elsewhere. */
    ustore_gds_automatic_k = 0,

    /** Direct transfers, refused with @c ustore_status_device_k where the driver is absent. */
    ustore_gds_direct_k = 1,

    /** cuFile's compatibility mode, which bounces through host pages it manages. */
    ustore_gds_compat_k = 2,

} ustore_gds_t;

/** The canonical spelling of a GPUDirect Storage mode, as @c ustore_gds_compat_k spells "compat", or
 *  "unrecognized". */
USTORE_EXPORT char const* ustore_gds_name(ustore_gds_t gds);

/** Reads a GPUDirect Storage mode back from its canonical spelling; @c ustore_gds_automatic_k when unrecognized. */
USTORE_EXPORT ustore_gds_t ustore_gds_from_name(ustore_bytes_view_t name);

/** The arguments of @c ustore_executor_for_vram. */
typedef struct ustore_executor_for_vram_t {

    /** Output for the new executor, which the caller frees with @c ustore_executor_free; @c NULL on failure. */
    ustore_executor_t* executor;

    /** Output for the outcome, final on return. */
    ustore_outcome_t* outcome;

    /** The CUDA ordinal every call and arena of this executor uses; an ordinal no device carries is refused with
     *  @c ustore_status_device_k. */
    int32_t device;

    /** The @b optional borrowed @c cudaStream_t on @c device, which must outlive the executor; @c NULL creates one.
     *  One stream per device is the whole design. */
    void* stream;

    /** The @b optional ceiling on the device scratch a call or a merge takes; zero lets scratch grow as calls ask. */
    ustore_size_t scratch_limit;

    /** How reads and compactions move bytes between the drives and the device. */
    ustore_gds_t gds;

    /** Deferred calls the executor holds at once, sizing its deferred table, where zero takes a default. */
    ustore_size_t deferred_limit;

} ustore_executor_for_vram_t;

/**
 *  @brief Opens one submission stream on @c device, or borrows the caller's, driven by one host thread.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  Every call queues on that stream in order, so one host thread keeps the device fed, and a multi-GPU caller makes
 *  one executor per device. Its calls read through an engine's device verbs, which refuse changes and the control
 *  plane, and every call no device tower serves yet, with @c ustore_status_unsupported_k, and arenas it makes live on
 *  its device. @c ustore_database_open_nvme takes one as its @c compactions, reading its device and scratch limit
 *  once.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c executor output is @c NULL, the @c gds mode is
 *    unknown, or ForkUnion refuses an argument.
 *  - @c ustore_status_unsupported_k The library was built without CUDA, the process may allocate on
 *    no memory domain, or the operating system denies ForkUnion an operation.
 *  - @c ustore_status_device_k No device carries the ordinal.
 *  - @c ustore_status_capacity_exhausted_k The @c deferred_limit is past 2^30.
 */
USTORE_EXPORT void ustore_executor_for_vram(ustore_executor_for_vram_t* arguments);

/** The arguments of @c ustore_executor_drain. */
typedef struct ustore_executor_drain_t {

    /** The executor to drain. */
    ustore_executor_t executor;

    /** Output for the outcome: an executor-level failure, such as a lost device, never one of a drained call. */
    ustore_outcome_t* outcome;

    /** What the threads do whenever the executor next goes idle, where the last drain wins. */
    ustore_mood_t mood;

} ustore_executor_drain_t;

/**
 *  @brief Waits for every call submitted before the drain began, so later submissions never delay it.
 *  @param[in] arguments Named arguments, whose outputs are written through their pointers.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c mood is unknown.
 *  - @c ustore_status_contended_k A callback drains the executor running it.
 */
USTORE_EXPORT void ustore_executor_drain(ustore_executor_drain_t* arguments);

/**
 *  @brief Drains every call, then frees the executor's threads, rings and scratch, and hands a borrowed pool back to
 *      its lender; @c NULL is safe, and it cannot fail, so takes no outcome.
 *  @param[in,out] executor The executor to be freed; invalid afterwards.
 *
 *  Waits until every call running on the executor, blocking or deferred, is final. Arenas it made stay valid, each
 *  keeping its memory until freed.
 */
USTORE_EXPORT void ustore_executor_free(ustore_executor_t executor);

/**
 *  @brief Memory the library writes every output whose length scales with a call into, reused across calls.
 *      @ref types_memory
 *
 *  Made by @c ustore_arena_init where its executor computes, and an executor whose device runs out of memory reports
 *  @c ustore_status_out_of_memory_k rather than falling back to the host. Each call given the arena rewinds it, which
 *  invalidates every output of the previous call, and refills it, growing only when the new outputs outsize
 *  everything it held, so a steady loop allocates once. Every output starts aligned to 64 bytes, as Arrow buffers
 *  do, so typed columns read in place. To keep one call's outputs alive past the next call, pass the next call
 *  another arena.
 *
 *  A @c NULL arena is refused with @c ustore_status_invalid_argument_k, and so is a call whose inputs point into the
 *  arena it is about to rewind. An arena serves one call at a time, so a second call naming it finishes
 *  @c ustore_status_contended_k, and a deferred one does so at submission. A call whose executor computes on host
 *  threads given an arena on a device, or on another device than the arena's, reports @c ustore_status_device_k.
 *  A blocking call borrows the arena until it returns, and a deferred one holds it until final. A call that fails,
 *  is cancelled, or has nothing to report leaves its outputs @c NULL.
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
 *  Nothing is allocated until a call writes into it. An arena may outlive the executor that made it: it keeps its own
 *  memory, and the device that memory lives on, until it is freed.
 *
 *  @par Statuses:
 *  - @c ustore_status_invalid_argument_k The @c arena output is @c NULL.
 */
USTORE_EXPORT void ustore_arena_init(ustore_arena_init_t* arguments);

/**
 *  @brief Frees the arena and every output in it; @c NULL is safe, and it cannot fail, so takes no outcome.
 *  @param[in,out] arena The arena to be freed; invalid afterwards.
 *
 *  Waits until every deferred call naming the arena is final.
 */
USTORE_EXPORT void ustore_arena_free(ustore_arena_t arena);

/**
 *  @brief Reports the version of the library the program runs against, which may differ from the macros it
 *      compiled against.
 *  @param[out] major Output for the major version.
 *  @param[out] minor Output for the minor version.
 *  @param[out] patch Output for the patch version.
 */
USTORE_EXPORT void ustore_version(int* major, int* minor, int* patch);

/** The version of the library the program runs against as a static string, like "1.0.0". */
USTORE_EXPORT char const* ustore_version_name(void);

#ifdef __cplusplus
}
#endif

#endif // UNUM_USTORE_TYPES_H
