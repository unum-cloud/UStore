/**
 *  @file src_stub/test.c
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief Checks the stand-in from plain C11 the way a binding suite relies on it.
 *
 *  Every refusal class comes in the ruled order, a failure leaves its outputs null or zero, and the
 *  claims of executors, arenas, transactions and cursors refuse a second call. The compaction plan
 *  opens, cancels, continues and finishes, and every modality round-trips its values.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>  // `printf`, `snprintf`, `fflush`, `remove`
#include <stdlib.h> // `getenv`, `exit`, `strtod`, `strtoull`, `mkdtemp`
#include <string.h> // `memcmp`, `memset`, `strcmp`, `strncmp`, `strlen`
#include <time.h>   // `time`, `clock`
#include <unistd.h> // `rmdir`, `write`, `STDERR_FILENO`

#include <inttypes.h> // `PRIu64`
#include <stdbool.h>  // `bool`
#include <stddef.h>   // `size_t`, `NULL`
#include <stdint.h>   // `uint32_t`, `uint64_t`

#include "ustore/ustore.h"

/*  region Shared C Checks  */
static size_t failed_checks = 0;
static size_t failed_checks_before_case = 0;
static size_t passed_cases = 0;
static size_t failed_cases = 0;
static size_t skipped_cases = 0;
static char const* current_case = NULL;
static char const* program_name = "";
static uint64_t test_seed = 42;
static double test_scale = 1.0;
static bool refuse_skips = false;

/** Fills the settings above from the file's own spelled-out variables and flags. */
static void read_test_settings(int argc, char** argv);

/** SplitMix64's finalizer, a bijection that spreads every input bit over all 64 output bits. */
static inline uint64_t split_mix_bits(uint64_t value) {
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

/** Writes @p text to the standard error through a raw @c write, which a signal handler may call. */
static void write_text(char const* text) { (void)!write(STDERR_FILENO, text, strlen(text)); }

/** Counts a failed check at @p file : @p line without stopping, so one run lists every mismatch. */
static inline bool expect_at(bool is_met, char const* file, int line, char const* what) {
    if (is_met) return true;
    fflush(stdout);
    char message[512];
    snprintf(message, sizeof message, "  check failed at %s:%d\n    %s\n", file, line, what);
    write_text(message);
    ++failed_checks;
    return false;
}

/** The text of a setting: its @p flag when given, else its environment @p variable , else NULL. */
static inline char const* setting_text(int argc, char** argv, char const* variable, char const* flag) {
    size_t const flag_length = strlen(flag);
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], flag) == 0)
            return index + 1 < argc && strncmp(argv[index + 1], "--", 2) != 0 ? argv[index + 1] : "1";
        if (strncmp(argv[index], flag, flag_length) == 0 && argv[index][flag_length] == '=')
            return argv[index] + flag_length + 1;
    }
    char const* text = getenv(variable);
    return text && *text ? text : NULL;
}

/** Exits with one line naming a setting that does not parse, so a bad value never runs anything. */
static inline void exit_unparsed(char const* variable, char const* text, char const* expected) {
    char message[512];
    snprintf(message, sizeof message, "%s=\"%s\" does not parse, expected %s\n", variable, text, expected);
    write_text(message);
    exit(1);
}

/** The run's seed: 42 when unset, drawn afresh for @c random , else any unsigned 64-bit value. */
static inline uint64_t read_seed_setting(int argc, char** argv, char const* variable, char const* flag) {
    char const* text = setting_text(argc, argv, variable, flag);
    char* end = NULL;
    if (!text) return 42;
    if (strcmp(text, "random") == 0) return split_mix_bits((uint64_t)time(NULL) ^ (uint64_t)clock());
    uint64_t const seed = (uint64_t)strtoull(text, &end, 10);
    if (*end != '\0' || *text == '-') exit_unparsed(variable, text, "an unsigned integer or random");
    return seed;
}

/** A positive real, @p fallback when unset. */
static inline double read_real_setting(int argc, char** argv, char const* variable, char const* flag, double fallback) {
    char const* text = setting_text(argc, argv, variable, flag);
    char* end = NULL;
    if (!text) return fallback;
    double const value = strtod(text, &end);
    if (*end != '\0' || !(value > 0)) exit_unparsed(variable, text, "a positive number like 0.1 or 10");
    return value;
}

/** A switch, @p fallback when unset: 0, false, off and no turn it off, anything else on. */
static inline bool read_flag_setting(int argc, char** argv, char const* variable, char const* flag, bool fallback) {
    char const* text = setting_text(argc, argv, variable, flag);
    if (!text) return fallback;
    return strcmp(text, "0") != 0 && strcmp(text, "false") != 0 && strcmp(text, "off") != 0 && strcmp(text, "no") != 0;
}

/** Reads the settings and prints the settings line, before the first case. */
static inline void start_test_run(int argc, char** argv) {
    program_name = argc != 0 ? argv[0] : "";
    read_test_settings(argc, argv);
    printf("- Seed: %" PRIu64 ", scale: %g\n", test_seed, test_scale);
}

/** Starts the case @p label. */
static inline void start_test_case(char const* label) {
    current_case = label;
    failed_checks_before_case = failed_checks;
}

/** Ends the case @ref start_test_case began: @p skip_reason when it skipped, NULL when it ran. */
static inline void finish_test_case(char const* skip_reason) {
    size_t const failed = failed_checks - failed_checks_before_case;
    if (skip_reason) {
        ++skipped_cases;
        printf("- %s ... skipped: %s\n", current_case, skip_reason);
    }
    else if (failed == 0) {
        ++passed_cases;
        printf("- %s ... ok\n", current_case);
    }
    else {
        ++failed_cases;
        fflush(stdout);
        char message[512];
        snprintf(message, sizeof message, "- %s ... FAILED: %zu failed check(s)\n", current_case, failed);
        write_text(message);
        snprintf(message, sizeof message, "  rerun: %s --seed %" PRIu64 "\n", program_name, test_seed);
        write_text(message);
    }
    current_case = NULL;
}

/** The exit code: 1 when a case or a check outside any case failed, or nothing ran; 3 when
 *  skips are refused and one happened; else 0. */
static inline int finish_test_run(void) {
    printf("%zu passed, %zu failed, %zu skipped, seed %" PRIu64 "\n", passed_cases, failed_cases, skipped_cases,
           test_seed);
    if (failed_cases != 0 || failed_checks != 0 || passed_cases + skipped_cases == 0) return 1;
    if (skipped_cases != 0 && refuse_skips) return 3;
    return 0;
}
/*  endregion Shared C Checks  */

static void read_test_settings(int argc, char** argv) {
    test_seed = read_seed_setting(argc, argv, "USTORE_SEED", "--seed");
    test_scale = read_real_setting(argc, argv, "USTORE_SCALE", "--scale", 1.0);
    refuse_skips = read_flag_setting(argc, argv, "USTORE_NO_SKIPS", "--no-skips", false);
}

#define expect_(condition) expect_at((condition), __FILE__, __LINE__, #condition)

/** Holds when @p outcome carries @p wanted ; otherwise counts a failed check at @p file : @p
 *  line naming both. */
static bool expect_status_at(ustore_outcome_t outcome, ustore_status_t wanted, char const* file, int line) {
    if (outcome.status == wanted) return true;
    char what[256];
    snprintf(what, sizeof(what), "expected %s, got %s: %s", ustore_status_name(wanted),
             ustore_status_name(outcome.status), outcome.message ? outcome.message : "no message");
    return expect_at(false, file, line, what);
}

static ustore_key_t key_from(uint8_t integer) {
    ustore_key_t key;
    memset(&key, 0, sizeof(key));
    key.uuid.bytes[15] = integer;
    return key;
}

static ustore_bytes_view_t bytes_of(char const* text) {
    ustore_bytes_view_t const bytes = {(uint8_t const*)text, strlen(text)};
    return bytes;
}

static int tape_holds(ustore_strings_view_t const* tape, size_t index, char const* text) {
    size_t const length = strlen(text);
    if (tape->layout == ustore_strings_u64_tape_k) {
        uint64_t const* offsets = tape->data.u64_tape.offsets;
        return offsets && offsets[index + 1] - offsets[index] == length &&
               memcmp(tape->data.u64_tape.data + offsets[index], text, length) == 0;
    }
    uint32_t const* offsets = tape->data.u32_tape.offsets;
    return offsets && offsets[index + 1] - offsets[index] == length &&
           memcmp(tape->data.u32_tape.data + offsets[index], text, length) == 0;
}

static void get_text(void const* handle, ustore_size_t const* indices, ustore_size_t count,
                     ustore_bytes_view_t* views) {
    char const* const* texts = (char const* const*)handle;
    for (ustore_size_t index = 0; index != count; ++index) views[index] = bytes_of(texts[indices[index]]);
}

static ustore_strings_view_t accessor_of(char const* const* texts) {
    ustore_strings_view_t view;
    memset(&view, 0, sizeof(view));
    view.layout = ustore_strings_accessor_k;
    view.data.accessor.get = &get_text;
    view.data.accessor.handle = texts;
    return view;
}

static ustore_strings_view_t u32_tape_of(char const* data, uint32_t const* offsets) {
    ustore_strings_view_t view;
    memset(&view, 0, sizeof(view));
    view.layout = ustore_strings_u32_tape_k;
    view.data.u32_tape.data = (uint8_t const*)data;
    view.data.u32_tape.offsets = offsets;
    return view;
}

#pragma region Reentrant Callbacks

/** What a progress callback does from inside the call it watches, to observe the claims
 *  that call holds. */
typedef struct reentry_t {
    ustore_database_t database;
    ustore_executor_t executor;
    ustore_arena_t arena;
    ustore_transaction_t transaction;
    ustore_blobs_cursor_t cursor;
    ustore_collection_id_t collection;
    ustore_status_t executor_status, arena_status, transaction_status, cursor_status, drain_status, ordered_status;
    ustore_size_t finished, total;
    int calls;
} reentry_t;

static ustore_continuation_t reenter(ustore_size_t finished, ustore_size_t total, void* state) {
    reentry_t* const reentry = (reentry_t*)state;
    reentry->finished = finished, reentry->total = total;
    if (reentry->calls++) return ustore_continue_k;
    ustore_key_t const key = key_from(1);
    ustore_outcome_t outcome = {0};
    ustore_octet_t const* presences = NULL;

    // Another call on the held executor finishes contended
    ustore_arena_t spare = NULL;
    ustore_arena_init_t spare_init = {.arena = &spare, .outcome = &outcome};
    ustore_arena_init(&spare_init);
    ustore_blobs_read_t read = {.database = reentry->database,
                                .executor = reentry->executor,
                                .outcome = &outcome,
                                .count = 1,
                                .collections = {&reentry->collection, 0},
                                .keys = {&key, 0},
                                .presences = &presences,
                                .arena = spare};
    ustore_blobs_read(&read);
    reentry->executor_status = outcome.status;

    // An invalid argument outranks the contention it meets
    read.arena = NULL;
    ustore_blobs_read(&read);
    reentry->ordered_status = outcome.status;

    // The held arena refuses a second call, whichever executor
    read.executor = NULL, read.arena = reentry->arena;
    ustore_blobs_read(&read);
    reentry->arena_status = outcome.status;

    // The held transaction refuses a second call
    if (reentry->transaction) {
        read.arena = spare, read.view = ustore_transaction_view(reentry->transaction, ustore_watch_keys_k);
        ustore_blobs_read(&read);
        reentry->transaction_status = outcome.status;
    }

    // The held cursor refuses a second pull
    if (reentry->cursor) {
        ustore_key_t const* keys = NULL;
        ustore_size_t written = 0;
        ustore_blobs_cursor_next_t next = {.database = reentry->database,
                                           .outcome = &outcome,
                                           .cursor = reentry->cursor,
                                           .keys = &keys,
                                           .written = &written,
                                           .arena = spare,
                                           .wanted = 1};
        ustore_blobs_cursor_next(&next);
        reentry->cursor_status = outcome.status;
    }

    // A drain from inside the call it would wait on is refused
    ustore_executor_drain_t drain = {.executor = reentry->executor, .outcome = &outcome};
    ustore_executor_drain(&drain);
    reentry->drain_status = outcome.status;
    ustore_arena_free(spare);
    return ustore_continue_k;
}

/** Cancels after @c allowed units. */
typedef struct canceller_t {
    ustore_size_t allowed, seen, decreased;
} canceller_t;

static ustore_continuation_t cancel_after(ustore_size_t finished, ustore_size_t total, void* state) {
    canceller_t* const canceller = (canceller_t*)state;
    canceller->decreased += finished > total;
    return ++canceller->seen > canceller->allowed ? ustore_cancel_k : ustore_continue_k;
}

#pragma endregion Reentrant Callbacks

/** Every closed set spells its members as the library names them, and reads each spelling back. */
static void test_enums_spell_and_parse_as_the_library_names_them(void) {
    ustore_status_t status = ustore_status_success_k;
    expect_(ustore_status_from_name(bytes_of("write-conflict"), &status) == ustore_status_success_k &&
            status == ustore_status_write_conflict_k);
    expect_(strcmp(ustore_status_name((ustore_status_t)99), "unrecognized") == 0);
    ustore_phase_t phase = ustore_phase_none_k;
    char const* const phases[] = {"none", "flush", "merge", "reclaim", "done"};
    for (int index = 0; index != 5; ++index) {
        expect_(strcmp(ustore_phase_name((ustore_phase_t)index), phases[index]) == 0);
        expect_(ustore_phase_from_name(bytes_of(phases[index]), &phase) == ustore_status_success_k &&
                phase == (ustore_phase_t)index);
    }
    expect_(ustore_phase_from_name(bytes_of("merging"), &phase) == ustore_status_invalid_argument_k);
    expect_(strcmp(ustore_cpu_class_name(ustore_cpu_class_aws_g5_k), "aws-g5") == 0);
    ustore_gpu_vendor_t vendor = ustore_gpu_vendor_unknown_k;
    expect_(strcmp(ustore_gpu_vendor_name(ustore_gpu_vendor_apple_k), "apple") == 0);
    expect_(ustore_gpu_vendor_from_name(bytes_of("apple"), &vendor) == ustore_status_success_k &&
            vendor == ustore_gpu_vendor_apple_k);
    ustore_gpu_class_t gpu_class = ustore_gpu_class_unknown_k;
    char const* const apple_classes[] = {"apple-metal7", "apple-metal9", "apple-metal10"};
    for (int index = 0; index != 3; ++index) {
        expect_(strcmp(ustore_gpu_class_name((ustore_gpu_class_t)(ustore_gpu_class_apple_metal7_k + index)),
                       apple_classes[index]) == 0);
        expect_(ustore_gpu_class_from_name(bytes_of(apple_classes[index]), &gpu_class) == ustore_status_success_k &&
                gpu_class == (ustore_gpu_class_t)(ustore_gpu_class_apple_metal7_k + index));
    }
    expect_(ustore_gpu_class_from_name(bytes_of("apple-metal8"), &gpu_class) == ustore_status_invalid_argument_k &&
            gpu_class == ustore_gpu_class_apple_metal10_k);
    expect_(strcmp(ustore_docs_field_type_name(ustore_docs_field_type_f16_k), "f16") == 0);
    expect_(strcmp(ustore_page_encoding_name(ustore_page_encoding_markdown_k), "markdown") == 0);
    expect_(strcmp(ustore_vertex_role_name(ustore_vertex_role_unknown_k), "unknown") == 0);
    expect_(strncmp(ustore_version_name(), "1.0.0", 5) == 0);
    int major = -1, minor = -1, patch = -1;
    ustore_version(&major, &minor, &patch);
    expect_(major == USTORE_VERSION_MAJOR && minor == USTORE_VERSION_MINOR && patch == USTORE_VERSION_PATCH);
}

/** Every call naming a null database answers uninitialized first, whatever else is
 *  wrong with it. */
static void test_null_databases_refuse_as_uninitialized(void) {
    ustore_outcome_t outcome = {0};
    ustore_octet_t const* presences = (ustore_octet_t const*)1;
    ustore_blobs_read_t read = {.outcome = &outcome, .presences = &presences};
    ustore_blobs_read(&read);
    expect_status_at(outcome, ustore_status_uninitialized_k, __FILE__, __LINE__);
    expect_(presences == NULL);
    ustore_blobs_write_t write = {.outcome = &outcome, .durability = (ustore_durability_t)7};
    ustore_blobs_write(&write);
    expect_status_at(outcome, ustore_status_uninitialized_k, __FILE__, __LINE__);
    ustore_database_description_t description = {.keys = 5};
    ustore_database_describe_t describe = {.outcome = &outcome, .description = &description};
    ustore_database_describe(&describe);
    expect_status_at(outcome, ustore_status_uninitialized_k, __FILE__, __LINE__);
    expect_(description.keys == 0);
    ustore_database_compact_t compact = {.outcome = &outcome};
    ustore_database_compact(&compact);
    expect_status_at(outcome, ustore_status_uninitialized_k, __FILE__, __LINE__);
    ustore_transaction_stage_t stage = {.outcome = &outcome};
    ustore_transaction_stage(&stage);
    expect_status_at(outcome, ustore_status_uninitialized_k, __FILE__, __LINE__);
    ustore_collection_create_t create = {.outcome = &outcome, .modality = (ustore_modality_t)42};
    ustore_collection_create(&create);
    expect_status_at(outcome, ustore_status_uninitialized_k, __FILE__, __LINE__);
    ustore_snapshot_save_t save = {.outcome = &outcome};
    ustore_snapshot_save(&save);
    expect_status_at(outcome, ustore_status_uninitialized_k, __FILE__, __LINE__);
    ustore_pages_read_t pages = {.outcome = &outcome};
    ustore_pages_read(&pages);
    expect_status_at(outcome, ustore_status_uninitialized_k, __FILE__, __LINE__);
}

/** The stand-in opens RAM databases alone, refusing every other engine with its outputs null. */
static void test_engines_beyond_ram_are_refused(void) {
    ustore_outcome_t outcome = {0};
    ustore_database_t database = (ustore_database_t)1;
    ustore_database_open_rocksdb_t rocksdb = {.database = &database,
                                              .outcome = &outcome,
                                              .directory = bytes_of("/tmp")};
    ustore_database_open_rocksdb(&rocksdb);
    expect_status_at(outcome, ustore_status_unsupported_k, __FILE__, __LINE__);
    expect_(database == NULL);
    ustore_database_open_leveldb_t leveldb = {.database = &database,
                                              .outcome = &outcome,
                                              .directory = bytes_of("/tmp")};
    ustore_database_open_leveldb(&leveldb);
    expect_status_at(outcome, ustore_status_unsupported_k, __FILE__, __LINE__);
    ustore_database_open_nvme_t nvme = {.database = &database, .outcome = &outcome, .directory = bytes_of("/tmp/x")};
    ustore_database_open_nvme(&nvme);
    expect_status_at(outcome, ustore_status_invalid_argument_k, __FILE__, __LINE__);
    nvme.directory = bytes_of("/tmp/x.ustore.json");
    ustore_database_open_nvme(&nvme);
    expect_status_at(outcome, ustore_status_unsupported_k, __FILE__, __LINE__);

    // A NUL byte in a path is an invalid argument, refused before the missing engine
    char const nul_path[] = {'/', 't', 'm', 'p', 0, 'x'};
    ustore_bytes_view_t const nul_view = {(uint8_t const*)nul_path, sizeof(nul_path)};
    rocksdb.directory = nul_view;
    ustore_database_open_rocksdb(&rocksdb);
    expect_status_at(outcome, ustore_status_invalid_argument_k, __FILE__, __LINE__);
    ustore_database_open_ram_t ram = {.database = &database, .outcome = &outcome, .directory = nul_view};
    ustore_database_open_ram(&ram);
    expect_status_at(outcome, ustore_status_invalid_argument_k, __FILE__, __LINE__);
    ram.directory = bytes_of("/no/such/directory/anywhere");
    ustore_database_open_ram(&ram);
    expect_status_at(outcome, ustore_status_not_found_k, __FILE__, __LINE__);

    // No GPU is listed, so no ordinal names one
    ustore_size_t devices = 7;
    ustore_gpu_devices_t list = {.outcome = &outcome, .devices_count = &devices};
    ustore_gpu_devices(&list);
    expect_(devices == 0);
    ustore_executor_t executor = (ustore_executor_t)1;
    ustore_executor_for_vram_t vram = {.executor = &executor, .outcome = &outcome};
    ustore_executor_for_vram(&vram);
    expect_status_at(outcome, ustore_status_device_k, __FILE__, __LINE__);
    expect_(executor == NULL);
    vram.gds = (ustore_gds_t)9;
    ustore_executor_for_vram(&vram);
    expect_status_at(outcome, ustore_status_invalid_argument_k, __FILE__, __LINE__);
    ustore_executor_for_ram_t foreign = {.executor = &executor,
                                         .outcome = &outcome,
                                         .cpu_class_ceiling = (ustore_cpu_class_t)99};
    ustore_executor_for_ram(&foreign);
    expect_status_at(outcome, ustore_status_invalid_argument_k, __FILE__, __LINE__);

    // A known ceiling narrows the pick to serial, the stand-in's one class, and is never refused
    foreign.cpu_class_ceiling = ustore_cpu_class_ibm_power10_k;
    ustore_executor_for_ram(&foreign);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    ustore_executor_free(executor);
    ustore_database_t capped = NULL;
    ustore_database_init_ram_t capping = {.database = &capped,
                                          .outcome = &outcome,
                                          .cpu_class_ceiling = ustore_cpu_class_intel_icelake_k};
    ustore_database_init_ram(&capping);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    ustore_database_description_t picked = {0};
    ustore_database_describe_t describe_picked = {.database = capped, .outcome = &outcome, .description = &picked};
    ustore_database_describe(&describe_picked);
    expect_(picked.cpu_class == ustore_cpu_class_serial_k);
    ustore_database_free(capped);
}

/** The compaction plan opens, cancels, continues and finishes, and refuses a second claim. */
static void check_compaction(ustore_database_t database, ustore_executor_t executor, ustore_executor_t other) {
    ustore_outcome_t outcome = {0};
    ustore_phase_progress_t phase = {0};

    // Without a plan the call runs one of its own to the end
    ustore_database_compact_t compact = {.database = database, .outcome = &outcome, .phase = &phase};
    ustore_database_compact(&compact);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    expect_(phase.phase == ustore_phase_done_k);

    // A plan opens only with an executor
    ustore_database_compact_plan_t plan = NULL;
    compact.plan = &plan;
    ustore_database_compact(&compact);
    expect_status_at(outcome, ustore_status_invalid_argument_k, __FILE__, __LINE__);
    expect_(plan == NULL && phase.phase == ustore_phase_none_k);

    // A null pointee opens a plan, which a cancel after its second unit leaves live
    canceller_t canceller = {1, 0, 0};
    compact.executor = executor, compact.progress = &cancel_after;
    compact.progress_state = &canceller;
    ustore_database_compact(&compact);
    expect_status_at(outcome, ustore_status_cancelled_k, __FILE__, __LINE__);
    expect_(plan != NULL && canceller.decreased == 0);
    expect_(phase.phase == ustore_phase_merge_k && phase.ordinal == 0 && phase.completed == 2 && phase.total >= 2);

    // One live plan per database: a second open and a one-shot are contended
    ustore_database_compact_plan_t second = NULL;
    ustore_database_compact_t again = {.database = database, .executor = other, .outcome = &outcome, .plan = &second};
    ustore_database_compact(&again);
    expect_status_at(outcome, ustore_status_contended_k, __FILE__, __LINE__);
    expect_(second == NULL);
    again.plan = NULL;
    ustore_database_compact(&again);
    expect_status_at(outcome, ustore_status_contended_k, __FILE__, __LINE__);

    // The plan remembers its executor and refuses another, then continues where it stopped
    compact.executor = other, compact.progress = NULL;
    ustore_database_compact(&compact);
    expect_status_at(outcome, ustore_status_invalid_argument_k, __FILE__, __LINE__);
    compact.executor = executor;
    ustore_database_compact(&compact);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    expect_(phase.phase == ustore_phase_done_k && phase.ordinal == 2);

    // The finish frees the plan whatever it answers, so another compaction opens
    ustore_database_compact_plan_finish_t finish = {.database = database,
                                                    .executor = other,
                                                    .outcome = &outcome,
                                                    .plan = plan};
    ustore_database_compact_plan_finish(&finish);
    expect_status_at(outcome, ustore_status_invalid_argument_k, __FILE__, __LINE__);
    compact.plan = NULL;
    ustore_database_compact(&compact);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    finish.plan = NULL;
    ustore_database_compact_plan_finish(&finish);
    expect_status_at(outcome, ustore_status_invalid_argument_k, __FILE__, __LINE__);
    plan = NULL, compact.plan = &plan;
    ustore_database_compact(&compact);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    finish.database = NULL, finish.executor = executor, finish.plan = plan;
    ustore_database_compact_plan_finish(&finish);
    expect_status_at(outcome, ustore_status_uninitialized_k, __FILE__, __LINE__);
    plan = NULL;
    ustore_database_compact(&compact);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    finish.database = database, finish.plan = plan;
    ustore_database_compact_plan_finish(&finish);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
}

/** Every modality round-trips its values on one RAM database, and the claims refuse
 *  a second call. */
static void test_every_modality_round_trips(void) {
    ustore_outcome_t outcome = {0};
    ustore_database_t database = NULL;
    ustore_database_init_ram_t init = {.database = &database, .outcome = &outcome};
    ustore_database_init_ram(&init);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    ustore_executor_t executor = NULL, other = NULL;
    ustore_executor_for_ram_t spawn = {.executor = &executor, .outcome = &outcome, .threads = 2};
    ustore_executor_for_ram(&spawn);
    spawn.executor = &other;
    ustore_executor_for_ram(&spawn);
    ustore_arena_t arena = NULL;
    ustore_arena_init_t arena_init = {.arena = &arena, .executor = executor, .outcome = &outcome};
    ustore_arena_init(&arena_init);
    if (!expect_(database && executor && arena)) return;

    ustore_collection_id_t blobs = {{{0}}}, docs = {{{0}}}, strings = {{{0}}}, graph = {{{0}}}, pages = {{{0}}};
    ustore_collection_create_t create = {.database = database, .outcome = &outcome, .collection = &blobs};
    create.name = bytes_of("_reserved"), create.modality = ustore_modality_blobs_k;
    ustore_collection_create(&create);
    expect_status_at(outcome, ustore_status_invalid_argument_k, __FILE__, __LINE__);
    create.name = bytes_of("blobs");
    ustore_collection_create(&create);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    ustore_collection_create(&create);
    expect_status_at(outcome, ustore_status_already_exists_k, __FILE__, __LINE__);
    ustore_collection_id_t const nil_collection = {{{0}}};
    expect_(memcmp(&blobs, &nil_collection, sizeof(nil_collection)) == 0);
    create.name = bytes_of("blobs"), create.collection = &blobs;
    {
        ustore_collection_id_t const* found = NULL;
        ustore_octet_t const* present = NULL;
        ustore_collection_find_t find = {.database = database,
                                         .outcome = &outcome,
                                         .count = 1,
                                         .names = accessor_of((char const* const[]) {"blobs"}),
                                         .presences = &present,
                                         .collections = &found,
                                         .arena = arena};
        ustore_collection_find(&find);
        expect_(found && present && (present[0] & 1));
        if (found) blobs = found[0];
    }
    char const* const names[4] = {"docs", "strings", "graph", "pages"};
    ustore_collection_id_t* const targets[4] = {&docs, &strings, &graph, &pages};
    ustore_modality_t const modalities[4] = {ustore_modality_docs_k, ustore_modality_strings_k, ustore_modality_graph_k,
                                             ustore_modality_pages_k};
    for (int index = 0; index != 4; ++index) {
        create.name = bytes_of(names[index]), create.modality = modalities[index], create.collection = targets[index];
        ustore_collection_create(&create);
        expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    }

    // Blobs: a write, the refusals of its arguments, and a read through a tape and an accessor
    ustore_key_t const keys[4] = {key_from(1), key_from(2), key_from(3), key_from(4)};
    char const* const texts[4] = {"red", "green", "blue", "gray"};
    ustore_blobs_write_t write = {.database = database,
                                  .outcome = &outcome,
                                  .count = 4,
                                  .collections = {&blobs, 0},
                                  .keys = {keys, sizeof(ustore_key_t)},
                                  .values = accessor_of(texts)};
    ustore_blobs_write(&write);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    uint32_t const descending[3] = {0, 4, 2};
    write.count = 2, write.values = u32_tape_of("redgreen", descending);
    ustore_blobs_write(&write);
    expect_status_at(outcome, ustore_status_invalid_argument_k, __FILE__, __LINE__);
    write.values = accessor_of(texts), write.values.layout = (ustore_strings_layout_t)9;
    ustore_blobs_write(&write);
    expect_status_at(outcome, ustore_status_invalid_argument_k, __FILE__, __LINE__);
    write.values = accessor_of(texts), write.watch = ustore_watch_none_k;
    ustore_blobs_write(&write);
    expect_status_at(outcome, ustore_status_invalid_argument_k, __FILE__, __LINE__);
    write.watch = ustore_watch_keys_k, write.collections.data = &docs;
    ustore_blobs_write(&write);
    expect_status_at(outcome, ustore_status_invalid_argument_k, __FILE__, __LINE__);
    write.collections.data = &blobs, write.count = 4;

    ustore_octet_t const* presences = NULL;
    ustore_length_t const* lengths = NULL;
    ustore_strings_view_t values = {0};
    ustore_blobs_read_t read = {.database = database,
                                .executor = executor,
                                .outcome = &outcome,
                                .completion = ustore_completion_deferred_k,
                                .count = 4,
                                .collections = {&blobs, 0},
                                .keys = {keys, sizeof(ustore_key_t)},
                                .presences = &presences,
                                .lengths = &lengths,
                                .values = &values,
                                .arena = arena};
    ustore_blobs_read(&read);
    expect_(ustore_outcome_wait(&outcome) == ustore_status_success_k);
    expect_(presences && presences[0] == 15 && lengths && lengths[1] == 5 && tape_holds(&values, 3, "gray"));
    read.keys.data = (ustore_key_t const*)lengths;
    ustore_blobs_read(&read);
    expect_(ustore_outcome_wait(&outcome) == ustore_status_invalid_argument_k);
    expect_(presences == NULL && lengths == NULL);
    read.keys.data = keys, read.completion = ustore_completion_blocking_k;

    // Reentrant calls from a progress callback see every claim the running scan holds, in
    // the ruled order
    ustore_transaction_t transaction = NULL;
    ustore_transaction_init_t begin = {.transaction = &transaction, .database = database, .outcome = &outcome};
    ustore_transaction_init(&begin);
    ustore_blobs_cursor_t cursor = NULL;
    ustore_blobs_cursor_init_t cursor_init = {.cursor = &cursor,
                                              .database = database,
                                              .outcome = &outcome,
                                              .collection = blobs};
    ustore_blobs_cursor_init(&cursor_init);
    reentry_t reentry = {.database = database,
                         .executor = executor,
                         .arena = arena,
                         .transaction = transaction,
                         .collection = blobs};
    ustore_length_t const* offsets = NULL;
    ustore_key_t const* found = NULL;
    ustore_blobs_scan_t scan = {.database = database,
                                .executor = executor,
                                .outcome = &outcome,
                                .view = ustore_transaction_view(transaction, ustore_watch_keys_k),
                                .count = 1,
                                .collections = {&blobs, 0},
                                .offsets = &offsets,
                                .keys = &found,
                                .arena = arena,
                                .wanted = 10,
                                .progress = &reenter,
                                .progress_state = &reentry};
    ustore_blobs_scan(&scan);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    expect_(offsets && offsets[1] == 4);
    expect_(reentry.executor_status == ustore_status_contended_k);
    expect_(reentry.ordered_status == ustore_status_invalid_argument_k);
    expect_(reentry.arena_status == ustore_status_contended_k);
    expect_(reentry.transaction_status == ustore_status_contended_k);
    expect_(reentry.drain_status == ustore_status_contended_k);
    expect_(reentry.finished == 1 && reentry.total == 1);

    // A pull holding its cursor refuses a second one
    reentry.calls = 0, reentry.cursor = cursor, reentry.transaction = NULL;
    ustore_size_t pulled = 0;
    ustore_blobs_cursor_next_t next = {.database = database,
                                       .outcome = &outcome,
                                       .cursor = cursor,
                                       .keys = &found,
                                       .written = &pulled,
                                       .arena = arena,
                                       .wanted = 3,
                                       .progress = &reenter,
                                       .progress_state = &reentry};
    ustore_blobs_cursor_next(&next);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    expect_(pulled == 3 && reentry.cursor_status == ustore_status_contended_k);
    next.progress = NULL;
    ustore_blobs_cursor_next(&next);
    expect_(pulled == 1);
    ustore_blobs_cursor_free(cursor);

    // Transactions: a staged one takes no writes, a flushed write inside one is refused, and the
    // later of two writers of one key conflicts
    write.transaction = transaction, write.durability = ustore_durability_flushed_k;
    ustore_blobs_write(&write);
    expect_status_at(outcome, ustore_status_invalid_argument_k, __FILE__, __LINE__);
    write.durability = ustore_durability_buffered_k, write.count = 1;
    ustore_blobs_write(&write);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    ustore_stamp_t read_stamp = 0, commit_stamp = 0;
    ustore_transaction_stage_t stage = {.database = database,
                                        .outcome = &outcome,
                                        .transaction = transaction,
                                        .read_stamp = &read_stamp};
    ustore_transaction_stage(&stage);
    ustore_blobs_write(&write);
    expect_status_at(outcome, ustore_status_invalid_argument_k, __FILE__, __LINE__);
    ustore_transaction_rollback_t rollback = {.database = database, .outcome = &outcome, .transaction = transaction};
    ustore_transaction_rollback(&rollback);
    write.transaction = NULL;
    ustore_blobs_write(&write);
    ustore_transaction_commit_t commit = {.database = database,
                                          .outcome = &outcome,
                                          .transaction = transaction,
                                          .commit_stamp = &commit_stamp};
    ustore_transaction_commit(&commit);
    expect_(outcome.status == ustore_status_write_conflict_k || outcome.status == ustore_status_read_conflict_k ||
            outcome.status == ustore_status_phantom_conflict_k);
    expect_(commit_stamp == 0);
    ustore_transaction_init(&begin);
    write.transaction = transaction;
    ustore_blobs_write(&write);
    ustore_transaction_commit(&commit);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    expect_(commit_stamp > read_stamp);
    ustore_transaction_free(transaction);

    // A snapshot keeps reading what it pinned while the head moves on
    ustore_snapshot_t snapshot = NULL;
    ustore_snapshot_init_t pin = {.snapshot = &snapshot, .database = database, .outcome = &outcome};
    ustore_snapshot_init(&pin);
    ustore_blobs_remove_t remove_range = {.database = database,
                                          .outcome = &outcome,
                                          .count = 1,
                                          .collections = {&blobs, 0},
                                          .keys = {keys, 0},
                                          .end_keys = {keys + 2, 0}};
    ustore_blobs_remove(&remove_range);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    read.view = ustore_snapshot_view(snapshot);
    ustore_blobs_read(&read);
    expect_(presences && presences[0] == 15);
    read.view = NULL;
    ustore_blobs_read(&read);
    expect_(presences && presences[0] == 12);
    ustore_collection_drop_t drop = {.database = database, .outcome = &outcome, .collection = blobs};
    ustore_collection_drop(&drop);
    expect_status_at(outcome, ustore_status_invalid_argument_k, __FILE__, __LINE__);
    ustore_snapshot_free(snapshot);

    // A feed opened at the head hands out the changes after it
    ustore_changes_cursor_t feed = NULL;
    ustore_changes_cursor_init_t feed_init = {.database = database,
                                              .outcome = &outcome,
                                              .collection = blobs,
                                              .since_stamp = 0,
                                              .cursor = &feed};
    ustore_changes_cursor_init(&feed_init);
    expect_status_at(outcome, ustore_status_write_conflict_k, __FILE__, __LINE__);
    ustore_database_description_t description = {0};
    ustore_database_describe_t describe = {.database = database, .outcome = &outcome, .description = &description};
    ustore_database_describe(&describe);
    ustore_stamp_t head = 0;
    ustore_snapshot_init_t peek = {.snapshot = &snapshot,
                                   .database = database,
                                   .outcome = &outcome,
                                   .read_stamp = &head};
    ustore_snapshot_init(&peek);
    ustore_snapshot_free(snapshot);
    feed_init.since_stamp = head;
    ustore_changes_cursor_init(&feed_init);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    write.transaction = NULL, write.count = 2;
    ustore_blobs_write(&write);
    ustore_size_t changes = 0;
    ustore_key_t const* changed = NULL;
    ustore_stamp_t const* stamps = NULL;
    ustore_change_kind_t const* kinds = NULL;
    ustore_changes_cursor_next_t pull = {.cursor = feed,
                                         .outcome = &outcome,
                                         .wanted = 10,
                                         .count = &changes,
                                         .keys = &changed,
                                         .stamps = &stamps,
                                         .kinds = &kinds,
                                         .values = &values,
                                         .arena = arena};
    ustore_changes_cursor_next(&pull);
    expect_(changes == 2 && kinds && kinds[0] == ustore_change_write_k && tape_holds(&values, 1, "green"));
    ustore_changes_cursor_free(feed);

    // Strings: a prefix scan in both directions
    char const* const words[4] = {"apple", "apricot", "banana", "avocado"};
    ustore_strings_write_t write_strings = {.database = database,
                                            .outcome = &outcome,
                                            .count = 4,
                                            .collections = {&strings, 0},
                                            .strings = accessor_of(words),
                                            .values = {keys, sizeof(ustore_key_t)}};
    ustore_strings_write(&write_strings);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    ustore_strings_view_t listed = {0};
    ustore_strings_scan_t scan_strings = {.database = database,
                                          .outcome = &outcome,
                                          .count = 1,
                                          .collections = {&strings, 0},
                                          .prefixes = accessor_of((char const* const[]) {"ap"}),
                                          .offsets = &offsets,
                                          .strings = &listed,
                                          .arena = arena,
                                          .wanted = 10};
    ustore_strings_scan(&scan_strings);
    expect_(offsets && offsets[1] == 2 && tape_holds(&listed, 0, "apple") && tape_holds(&listed, 1, "apricot"));
    scan_strings.direction = ustore_scan_descending_k;
    ustore_strings_scan(&scan_strings);
    expect_(offsets && offsets[1] == 2 && tape_holds(&listed, 0, "apricot"));

    // Documents: a patch, a merge and a field read
    ustore_key_t const doc_key = key_from(9);
    char const* const documents[1] = {"{\"name\":\"ada\",\"tags\":[1,2]}"};
    ustore_docs_write_t write_docs = {.database = database,
                                      .outcome = &outcome,
                                      .count = 1,
                                      .collections = {&docs, 0},
                                      .keys = {&doc_key, 0},
                                      .values = accessor_of(documents)};
    ustore_docs_write(&write_docs);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    char const* const patch[1] = {"[{\"op\":\"add\",\"path\":\"/tags/-\",\"value\":3}]"};
    write_docs.values = accessor_of(patch), write_docs.modification = ustore_docs_modify_patch_k;
    ustore_docs_write(&write_docs);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    char const* const merge[1] = {"{\"name\":null,\"year\":1815}"};
    write_docs.values = accessor_of(merge), write_docs.modification = ustore_docs_modify_merge_k;
    ustore_docs_write(&write_docs);
    ustore_docs_read_t read_docs = {.database = database,
                                    .outcome = &outcome,
                                    .count = 1,
                                    .collections = {&docs, 0},
                                    .keys = {&doc_key, 0},
                                    .values = &values,
                                    .arena = arena};
    ustore_docs_read(&read_docs);
    expect_(tape_holds(&values, 0, "{\"tags\":[1,2,3],\"year\":1815}"));
    read_docs.outputs_type = ustore_docs_field_type_f64_k;
    ustore_docs_read(&read_docs);
    expect_status_at(outcome, ustore_status_unsupported_k, __FILE__, __LINE__);

    // Graphs: removing a vertex removes its edges at both ends
    ustore_graph_write_edges_t write_edges = {.database = database,
                                              .outcome = &outcome,
                                              .count = 3,
                                              .collections = {&graph, 0},
                                              .sources = {keys, 0},
                                              .targets = {keys + 1, sizeof(ustore_key_t)}};
    ustore_graph_write_edges(&write_edges);
    ustore_graph_remove_vertices_t remove_vertex = {.database = database,
                                                    .outcome = &outcome,
                                                    .count = 1,
                                                    .collections = {&graph, 0},
                                                    .vertices = {keys + 1, 0}};
    ustore_graph_remove_vertices(&remove_vertex);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    ustore_vertex_degree_t const* degrees = NULL;
    ustore_graph_read_edges_t read_edges = {.database = database,
                                            .outcome = &outcome,
                                            .count = 3,
                                            .collections = {&graph, 0},
                                            .vertices = {keys, sizeof(ustore_key_t)},
                                            .degrees_per_vertex = &degrees,
                                            .arena = arena};
    ustore_graph_read_edges(&read_edges);
    expect_(degrees && degrees[0] == 2 && degrees[1] == ustore_vertex_degree_missing_k && degrees[2] == 1);

    // Pages: an attribute set, a splice and a render as text
    ustore_key_t const page_key = key_from(10);
    char const* const markups[1] = {"<doc><p id=\"a\">one</p></doc>"};
    ustore_pages_import_t import_page = {.database = database,
                                         .outcome = &outcome,
                                         .count = 1,
                                         .collections = {&pages, 0},
                                         .keys = {&page_key, 0},
                                         .values = accessor_of(markups),
                                         .encoding = ustore_page_encoding_xml_k};
    ustore_pages_import(&import_page);
    expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
    import_page.encoding = ustore_page_encoding_html_k;
    ustore_pages_import(&import_page);
    expect_status_at(outcome, ustore_status_unsupported_k, __FILE__, __LINE__);
    ustore_pages_read_t read_page = {.database = database,
                                     .outcome = &outcome,
                                     .count = 1,
                                     .collections = {&pages, 0},
                                     .keys = {&page_key, 0},
                                     .values = &values,
                                     .arena = arena,
                                     .outputs_encoding = ustore_page_encoding_xml_k};
    ustore_pages_read(&read_page);
    expect_(tape_holds(&values, 0, "<doc><p id=\"a\">one</p></doc>"));
    read_page.outputs_encoding = ustore_page_encoding_text_k;
    ustore_pages_read(&read_page);
    expect_(tape_holds(&values, 0, "one"));

    // Arrow: keys a scan wrote export in place and convert back
    scan.view = NULL, scan.progress = NULL, scan.executor = NULL;
    ustore_blobs_scan(&scan);
    struct ArrowSchema schema = {0};
    struct ArrowDeviceArray column = {0};
    expect_(ustore_keys_view_to_arrow((ustore_keys_view_t) {found, sizeof(ustore_key_t)}, offsets[1], arena, &schema,
                                      &column) == ustore_status_success_k);
    ustore_keys_view_t converted = {0};
    ustore_size_t converted_count = 0;
    expect_(ustore_keys_view_from_arrow(&schema, &column.array, &converted, &converted_count) ==
                ustore_status_success_k &&
            converted_count == offsets[1]);
    if (column.array.release) column.array.release(&column.array);
    if (schema.release) schema.release(&schema);

    check_compaction(database, executor, other);

    // An image round-trips through a directory the RAM engine serves in place
    char directory[] = "/tmp/ustore-stub-XXXXXX";
    if (mkdtemp(directory)) {
        ustore_database_t persisted = NULL;
        ustore_database_open_ram_t open = {.database = &persisted,
                                           .outcome = &outcome,
                                           .directory = bytes_of(directory)};
        ustore_database_open_ram(&open);
        expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
        ustore_collection_id_t kept = {{{0}}};
        ustore_collection_create_t create_kept = {.database = persisted,
                                                  .outcome = &outcome,
                                                  .name = bytes_of("kept"),
                                                  .modality = ustore_modality_blobs_k,
                                                  .collection = &kept};
        ustore_collection_create(&create_kept);
        write.database = persisted, write.collections.data = &kept, write.durability = ustore_durability_flushed_k;
        ustore_blobs_write(&write);
        expect_status_at(outcome, ustore_status_success_k, __FILE__, __LINE__);
        ustore_database_free(persisted);
        persisted = NULL;
        ustore_database_open_ram(&open);
        read.database = persisted, read.collections.data = &kept, read.count = 2;
        ustore_blobs_read(&read);
        expect_(presences && presences[0] == 3 && tape_holds(&values, 0, "red"));
        ustore_database_free(persisted);
        char path[64];
        snprintf(path, sizeof(path), "%s/ram.image", directory);
        remove(path);
        rmdir(directory);
    }

    ustore_arena_free(arena);
    ustore_executor_free(executor);
    ustore_executor_free(other);
    ustore_database_free(database);
}

int main(int argc, char** argv) {
    start_test_run(argc, argv);
    start_test_case("test_enums_spell_and_parse_as_the_library_names_them");
    test_enums_spell_and_parse_as_the_library_names_them();
    finish_test_case(NULL);
    start_test_case("test_null_databases_refuse_as_uninitialized");
    test_null_databases_refuse_as_uninitialized();
    finish_test_case(NULL);
    start_test_case("test_engines_beyond_ram_are_refused");
    test_engines_beyond_ram_are_refused();
    finish_test_case(NULL);
    start_test_case("test_every_modality_round_trips");
    test_every_modality_round_trips();
    finish_test_case(NULL);
    return finish_test_run();
}

#undef expect_
