/**
 *  @file c/test.c
 *  @author Ash Vardanian
 *  @date October 13, 2026
 *  @brief Calls the C ABI from plain C11 once per shape of its arguments, so a header that only a C++ compiler accepts
 *      fails to build, and a shape the library misreads fails the run.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <stdio.h>  // `snprintf`, `printf`, `fflush`
#include <stdlib.h> // `getenv`, `exit`, `strtod`, `strtoull`
#include <string.h> // `memcmp`, `memset`, `strcmp`, `strncmp`, `strlen`
#include <time.h>   // `time`, `clock`

#include <inttypes.h> // `PRIu64`
#include <stdbool.h>  // `bool`
#include <stddef.h>   // `size_t`, `NULL`
#include <stdint.h>   // `uint32_t`, `uint64_t`

#include "ustore/ustore.h"

#if USTORE_OS_WINDOWS_
#include <io.h> // `_write`
#else
#include <unistd.h> // `write`, `STDERR_FILENO`
#endif

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
static void write_text(char const* text) {
#if USTORE_OS_WINDOWS_
    (void)!_write(2, text, (unsigned)strlen(text));
#else
    (void)!write(STDERR_FILENO, text, strlen(text));
#endif
}

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

/** Holds when @p outcome reports success; otherwise counts a failed check at @p file : @p line
 *  with its message. */
static bool expect_success_at(ustore_outcome_t const* outcome, char const* file, int line) {
    return expect_at(outcome->status == ustore_status_success_k, file, line,
                     outcome->message ? outcome->message : ustore_status_name(outcome->status));
}

/** The key spelling the small @p integer as its big-endian bytes. */
static ustore_key_t key_from(uint8_t integer) {
    ustore_key_t key;
    memset(&key, 0, sizeof(key));
    key.uuid.bytes[15] = integer;
    return key;
}

/** The bytes of the null-terminated @p text, without its terminator. */
static ustore_bytes_view_t bytes_of(char const* text) {
    ustore_bytes_view_t const bytes = {(uint8_t const*)text, strlen(text)};
    return bytes;
}

/** Whether string @p index of the output @p tape spells @p text. */
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

/** Writes the views of the null-terminated strings at @p indices of the array @p handle points to, as a binding over
 *  its own runtime strings would. */
static void get_text(void const* handle, ustore_size_t const* indices, ustore_size_t count,
                     ustore_bytes_view_t* views) {
    char const* const* texts = (char const* const*)handle;
    for (ustore_size_t index = 0; index != count; ++index) views[index] = bytes_of(texts[indices[index]]);
}

/** A tape of 32-bit offsets over @p data, as Apache Arrow's @c Binary holds strings. */
static ustore_strings_view_t u32_tape_of(char const* data, uint32_t const* offsets) {
    ustore_strings_view_t view;
    memset(&view, 0, sizeof(view));
    view.layout = ustore_strings_u32_tape_k;
    view.data.u32_tape.data = (uint8_t const*)data;
    view.data.u32_tape.offsets = offsets;
    return view;
}

/** A tape of 64-bit offsets over @p data, as Apache Arrow's @c LargeBinary holds strings. */
static ustore_strings_view_t u64_tape_of(char const* data, uint64_t const* offsets) {
    ustore_strings_view_t view;
    memset(&view, 0, sizeof(view));
    view.layout = ustore_strings_u64_tape_k;
    view.data.u64_tape.data = (uint8_t const*)data;
    view.data.u64_tape.offsets = offsets;
    return view;
}

/** An accessor over the null-terminated strings of @p texts. */
static ustore_strings_view_t accessor_of(char const* const* texts) {
    ustore_strings_view_t view;
    memset(&view, 0, sizeof(view));
    view.layout = ustore_strings_accessor_k;
    view.data.accessor.get = &get_text;
    view.data.accessor.handle = texts;
    return view;
}

/** Counts each report in the @c size_t behind @p state and cancels the call at a sound one, so a
 *  compaction holding a plan stops after one unit. */
static ustore_continuation_t cancel_at_first(ustore_size_t finished, ustore_size_t total, void* state) {
    ++*(size_t*)state;
    return finished <= total ? ustore_cancel_k : ustore_continue_k;
}

/** The Quickstart of c/README.md, line by line. The README's @c main is this function; its
 *  @c assert calls are @c expect_ checks, since a Release build drops asserts, and each also tests
 *  the pointer it reads, since a failed call leaves its outputs null; every call's outcome is
 *  checked as it lands, where the README checks the commit. */
static void test_readme_quickstart(void) {
    ustore_outcome_t outcome = {0};
    ustore_database_t db = NULL;
    ustore_arena_t arena = NULL;
    ustore_transaction_t txn = NULL;
    ustore_database_init_ram(&(ustore_database_init_ram_t) {.database = &db, .outcome = &outcome});
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_arena_init(&(ustore_arena_init_t) {.arena = &arena, .outcome = &outcome});
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_collection_id_t posters = {0}, movies = {0}, similar = {0};
    ustore_collection_create(&(ustore_collection_create_t) {.database = db,
                                                            .outcome = &outcome,
                                                            .modality = ustore_modality_blobs_k,
                                                            .collection = &posters});
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_collection_create(&(ustore_collection_create_t) {.database = db,
                                                            .outcome = &outcome,
                                                            .modality = ustore_modality_docs_k,
                                                            .collection = &movies});
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_collection_create(&(ustore_collection_create_t) {.database = db,
                                                            .outcome = &outcome,
                                                            .modality = ustore_modality_graph_k,
                                                            .collection = &similar});
    expect_success_at(&outcome, __FILE__, __LINE__);

    // One transaction writes a blob, a document and an edge, and commits all three or none
    ustore_key_t const avatar = {.uuid.bytes[15] = 1}, titanic = {.uuid.bytes[15] = 2};
    char const poster[] = "\x89PNG", movie[] = "{\"title\":\"Avatar\",\"year\":2009}";
    uint32_t const poster_offsets[2] = {0, sizeof(poster) - 1}, movie_offsets[2] = {0, sizeof(movie) - 1};
    ustore_transaction_init(&(ustore_transaction_init_t) {.transaction = &txn, .database = db, .outcome = &outcome});
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_blobs_write(&(ustore_blobs_write_t) {
        .database = db,
        .outcome = &outcome,
        .transaction = txn,
        .count = 1,
        .collections = {&posters, 0},
        .keys = {&avatar, 0},
        .values = {.layout = ustore_strings_u32_tape_k, .data.u32_tape = {(uint8_t const*)poster, poster_offsets}}});
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_docs_write(&(ustore_docs_write_t) {
        .database = db,
        .outcome = &outcome,
        .transaction = txn,
        .count = 1,
        .collections = {&movies, 0},
        .keys = {&avatar, 0},
        .values = {.layout = ustore_strings_u32_tape_k, .data.u32_tape = {(uint8_t const*)movie, movie_offsets}}});
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_graph_write_edges(&(ustore_graph_write_edges_t) {.database = db,
                                                            .outcome = &outcome,
                                                            .transaction = txn,
                                                            .count = 1,
                                                            .collections = {&similar, 0},
                                                            .sources = {&avatar, 0},
                                                            .targets = {&titanic, 0}});
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_transaction_commit(&(ustore_transaction_commit_t) {.database = db, .outcome = &outcome, .transaction = txn});
    expect_success_at(&outcome, __FILE__, __LINE__);

    // Reads at the head see the commit: the poster, the title and the one edge leaving Avatar
    ustore_strings_view_t read = {0};
    ustore_blobs_read(&(ustore_blobs_read_t) {.database = db,
                                              .outcome = &outcome,
                                              .count = 1,
                                              .collections = {&posters, 0},
                                              .keys = {&avatar, 0},
                                              .values = &read,
                                              .arena = arena});
    expect_success_at(&outcome, __FILE__, __LINE__);
    expect_(read.data.u32_tape.offsets && read.data.u32_tape.offsets[1] == 4 &&
            memcmp(read.data.u32_tape.data, poster, 4) == 0);
    uint32_t const title_offsets[2] = {0, 5};
    ustore_docs_read(&(ustore_docs_read_t) {
        .database = db,
        .outcome = &outcome,
        .count = 1,
        .collections = {&movies, 0},
        .keys = {&avatar, 0},
        .fields = {.layout = ustore_strings_u32_tape_k, .data.u32_tape = {(uint8_t const*)"title", title_offsets}},
        .values = &read,
        .arena = arena,
        .outputs_type = ustore_docs_field_type_string_k});
    expect_success_at(&outcome, __FILE__, __LINE__);
    expect_(read.data.u32_tape.data && strcmp((char const*)read.data.u32_tape.data, "Avatar") == 0);
    ustore_vertex_degree_t const* degrees = NULL;
    ustore_length_t const* edges_offsets = NULL;
    ustore_key_t const* edges = NULL;
    ustore_graph_read_edges(&(ustore_graph_read_edges_t) {.database = db,
                                                          .outcome = &outcome,
                                                          .count = 1,
                                                          .collections = {&similar, 0},
                                                          .vertices = {&avatar, 0},
                                                          .degrees_per_vertex = &degrees,
                                                          .edges_offsets = &edges_offsets,
                                                          .edges_per_vertex = &edges,
                                                          .arena = arena});
    expect_success_at(&outcome, __FILE__, __LINE__);
    expect_(degrees && degrees[0] == 1 && memcmp(&edges[1], &titanic, sizeof(titanic)) == 0);
    ustore_transaction_free(txn);
    ustore_arena_free(arena);
    ustore_database_free(db);
}

/** Every call of the C ABI once per shape of its arguments, on one in-memory database. */
static void test_every_argument_shape_lands(void) {
    ustore_outcome_t outcome = {0};

    // The in-memory engine starts empty and persists nowhere
    ustore_database_t database = NULL;
    ustore_database_init_ram_t init = {.database = &database, .outcome = &outcome};
    ustore_database_init_ram(&init);
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_arena_t arena = NULL;
    ustore_arena_init_t arena_init = {.arena = &arena, .outcome = &outcome};
    ustore_arena_init(&arena_init);
    if (!expect_success_at(&outcome, __FILE__, __LINE__)) return;

    // An arena made for the calling thread lives on host pages
    ustore_arena_description_t arena_description = {0};
    ustore_arena_describe_t arena_describe = {.arena = arena, .outcome = &outcome, .description = &arena_description};
    ustore_arena_describe(&arena_describe);
    expect_success_at(&outcome, __FILE__, __LINE__);
    expect_(arena_description.residency == ustore_residency_ram_k);
    expect_(arena_description.device == -1);

    // No collection exists until one is created, and every call names the collections it addresses
    ustore_collection_id_t blobs = {{{0}}};
    ustore_collection_create_t create_blobs = {.database = database,
                                               .outcome = &outcome,
                                               .name = bytes_of("values"),
                                               .modality = ustore_modality_blobs_k,
                                               .collection = &blobs};
    ustore_collection_create(&create_blobs);
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_collections_view_t const blobs_view = {&blobs, 0};

    // The same three values as a u32 tape, a u64 tape and an accessor, each written over the last
    ustore_key_t const keys[3] = {key_from(1), key_from(2), key_from(3)};
    ustore_keys_view_t const keys_view = {keys, sizeof(ustore_key_t)};
    char const joined[] = "redgreenblue";
    char const* const texts[3] = {"red", "green", "blue"};
    uint32_t const narrow_offsets[4] = {0, 3, 8, 12};
    uint64_t const wide_offsets[4] = {0, 3, 8, 12};
    ustore_strings_view_t const narrow_tape = u32_tape_of(joined, narrow_offsets);
    ustore_blobs_write_t write = {.database = database,
                                  .outcome = &outcome,
                                  .count = 3,
                                  .collections = blobs_view,
                                  .keys = keys_view,
                                  .values = narrow_tape};
    ustore_blobs_write(&write);
    expect_success_at(&outcome, __FILE__, __LINE__);
    write.values = u64_tape_of(joined, wide_offsets);
    ustore_blobs_write(&write);
    expect_success_at(&outcome, __FILE__, __LINE__);
    write.values = accessor_of(texts);
    ustore_blobs_write(&write);
    expect_success_at(&outcome, __FILE__, __LINE__);

    // A read answers presences, lengths and a tape of values in the arena
    ustore_octet_t const* presences = NULL;
    ustore_length_t const* lengths = NULL;
    ustore_strings_view_t values = {0};
    ustore_blobs_read_t read = {.database = database,
                                .outcome = &outcome,
                                .count = 3,
                                .collections = blobs_view,
                                .keys = keys_view,
                                .presences = &presences,
                                .lengths = &lengths,
                                .values = &values,
                                .arena = arena};
    ustore_blobs_read(&read);
    expect_success_at(&outcome, __FILE__, __LINE__);
    expect_(presences && presences[0] == 7 && lengths && lengths[1] == 5);
    expect_(tape_holds(&values, 0, "red") && tape_holds(&values, 2, "blue"));

    // A transaction or snapshot of one database is refused through another of the same engine
    ustore_database_t other = NULL;
    ustore_database_init_ram_t init_other = {.database = &other, .outcome = &outcome};
    ustore_database_init_ram(&init_other);
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_key_t const foreign_key = key_from(9);
    ustore_transaction_t foreign = NULL;
    ustore_transaction_init_t begin_foreign = {.transaction = &foreign, .database = database, .outcome = &outcome};
    ustore_transaction_init(&begin_foreign);
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_blobs_write_t foreign_write = {.database = database,
                                          .outcome = &outcome,
                                          .transaction = foreign,
                                          .count = 1,
                                          .collections = blobs_view,
                                          .keys = {&foreign_key, 0},
                                          .values = narrow_tape};
    ustore_blobs_write(&foreign_write);
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_transaction_commit_t misdirected = {.database = other, .outcome = &outcome, .transaction = foreign};
    ustore_transaction_commit(&misdirected);
    expect_(outcome.status == ustore_status_invalid_argument_k);
    ustore_blobs_read_t foreign_read = {.database = database,
                                        .outcome = &outcome,
                                        .count = 1,
                                        .collections = blobs_view,
                                        .keys = {&foreign_key, 0},
                                        .presences = &presences,
                                        .arena = arena};
    ustore_blobs_read(&foreign_read);
    expect_success_at(&outcome, __FILE__, __LINE__);
    expect_(presences && (presences[0] & 1) == 0);
    ustore_transaction_free(foreign);
    ustore_snapshot_t pinned = NULL;
    ustore_snapshot_init_t pin = {.snapshot = &pinned, .database = database, .outcome = &outcome};
    ustore_snapshot_init(&pin);
    expect_success_at(&outcome, __FILE__, __LINE__);
    foreign_read.database = other;
    foreign_read.view = ustore_snapshot_view(pinned);
    ustore_blobs_read(&foreign_read);
    expect_(outcome.status == ustore_status_invalid_argument_k);
    ustore_snapshot_free(pinned);
    ustore_database_free(other);

    // A scan repeats one start key for its single task through a zero stride
    ustore_length_t const* scan_offsets = NULL;
    ustore_key_t const* found_keys = NULL;
    ustore_blobs_scan_t scan = {.database = database,
                                .outcome = &outcome,
                                .count = 1,
                                .collections = blobs_view,
                                .start_keys = {keys, 0},
                                .offsets = &scan_offsets,
                                .keys = &found_keys,
                                .arena = arena,
                                .wanted = 3};
    ustore_blobs_scan(&scan);
    expect_success_at(&outcome, __FILE__, __LINE__);
    expect_(scan_offsets && found_keys && scan_offsets[1] == 3);

    // A collection takes its name as bytes, and a listing answers the names as a tape
    ustore_collection_id_t colors = {{{0}}};
    ustore_collection_create_t create = {.database = database,
                                         .outcome = &outcome,
                                         .name = bytes_of("colors"),
                                         .modality = ustore_modality_strings_k,
                                         .collection = &colors};
    ustore_collection_create(&create);
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_size_t listed = 0;
    ustore_strings_view_t names = {0};
    ustore_collection_list_t list = {.database = database,
                                     .outcome = &outcome,
                                     .names = &names,
                                     .written = &listed,
                                     .arena = arena};
    ustore_collection_list(&list);
    expect_success_at(&outcome, __FILE__, __LINE__);
    expect_(listed == 2 && (tape_holds(&names, 0, "colors") || tape_holds(&names, 1, "colors")));

    // Strings read through an accessor map onto strided keys, and read back through a tape of the same strings
    ustore_collections_view_t const colors_view = {&colors, 0};
    ustore_strings_write_t write_strings = {.database = database,
                                            .outcome = &outcome,
                                            .count = 3,
                                            .collections = colors_view,
                                            .strings = accessor_of(texts),
                                            .values = keys_view};
    ustore_strings_write(&write_strings);
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_key_t const* color_keys = NULL;
    ustore_strings_read_t read_strings = {.database = database,
                                          .outcome = &outcome,
                                          .count = 3,
                                          .collections = colors_view,
                                          .strings = narrow_tape,
                                          .presences = &presences,
                                          .values = &color_keys,
                                          .arena = arena};
    ustore_strings_read(&read_strings);
    expect_success_at(&outcome, __FILE__, __LINE__);
    expect_(presences && presences[0] == 7 && color_keys);
    expect_(color_keys && memcmp(color_keys, keys, sizeof(keys)) == 0);

    // A cursor pulls the keys of a collection with their values, then nothing once a page came back short
    ustore_blobs_cursor_t keys_cursor = NULL;
    ustore_blobs_cursor_init_t keys_cursor_init = {.cursor = &keys_cursor,
                                                   .database = database,
                                                   .outcome = &outcome,
                                                   .collection = blobs};
    ustore_blobs_cursor_init(&keys_cursor_init);
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_size_t pulled = 0;
    ustore_blobs_cursor_next_t keys_next = {.database = database,
                                            .outcome = &outcome,
                                            .cursor = keys_cursor,
                                            .keys = &found_keys,
                                            .values = &values,
                                            .written = &pulled,
                                            .arena = arena,
                                            .wanted = 2};
    ustore_blobs_cursor_next(&keys_next);
    expect_success_at(&outcome, __FILE__, __LINE__);
    expect_(pulled == 2 && tape_holds(&values, 1, "green"));
    ustore_blobs_cursor_next(&keys_next);
    expect_success_at(&outcome, __FILE__, __LINE__);
    expect_(pulled == 1 && tape_holds(&values, 0, "blue"));
    ustore_blobs_cursor_next(&keys_next);
    expect_(pulled == 0 && found_keys == NULL);
    ustore_blobs_cursor_free(keys_cursor);

    // A tape a read wrote exports as an Arrow column reading its arena in place until the
    // arena's next call
    ustore_arena_t exported = NULL;
    ustore_arena_init_t exported_init = {.arena = &exported, .outcome = &outcome};
    ustore_arena_init(&exported_init);
    read.arena = exported;
    ustore_blobs_read(&read);
    expect_success_at(&outcome, __FILE__, __LINE__);
    struct ArrowSchema schema = {0};
    struct ArrowDeviceArray column = {0};
    expect_(ustore_strings_view_to_arrow(values, 3, ustore_strings_u32_tape_k, exported, &schema, &column) ==
            ustore_status_success_k);
    ustore_strings_view_t converted = {0};
    ustore_size_t converted_count = 0;
    expect_(ustore_strings_view_from_arrow(&schema, &column.array, &converted, &converted_count) ==
                ustore_status_success_k &&
            converted_count == 3 && tape_holds(&converted, 2, "blue"));
    if (column.array.release) column.array.release(&column.array);
    if (schema.release) schema.release(&schema);
    ustore_arena_free(exported);
    read.arena = arena;

    // A strings cursor copies its prefix and walks the strings it starts, with the keys they map onto
    ustore_strings_cursor_t strings_cursor = NULL;
    ustore_strings_cursor_init_t strings_cursor_init = {.cursor = &strings_cursor,
                                                        .database = database,
                                                        .outcome = &outcome,
                                                        .collection = colors,
                                                        .prefix = bytes_of("gr")};
    ustore_strings_cursor_init(&strings_cursor_init);
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_strings_view_t walked = {0};
    ustore_key_t const* walked_keys = NULL;
    ustore_strings_cursor_next_t strings_next = {.database = database,
                                                 .outcome = &outcome,
                                                 .cursor = strings_cursor,
                                                 .strings = &walked,
                                                 .values = &walked_keys,
                                                 .written = &pulled,
                                                 .arena = arena,
                                                 .wanted = 8};
    ustore_strings_cursor_next(&strings_next);
    expect_success_at(&outcome, __FILE__, __LINE__);
    expect_(pulled == 1 && tape_holds(&walked, 0, "green"));
    expect_(walked_keys && memcmp(walked_keys, &keys[1], sizeof(ustore_key_t)) == 0);
    ustore_strings_cursor_free(strings_cursor);

    // An executor serves every memory domain the process may allocate on when its options name none
    ustore_executor_t executor = NULL;
    ustore_executor_for_ram_t spawn = {.executor = &executor, .outcome = &outcome, .ram = {NULL, 0}, .threads = 1};
    ustore_executor_for_ram(&spawn);
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_executor_free(executor);

    // Edges take strided sources and targets, and a read takes one role for every vertex
    ustore_collection_id_t graph = {{{0}}};
    create.name = bytes_of("graph");
    create.modality = ustore_modality_graph_k;
    create.collection = &graph;
    ustore_collection_create(&create);
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_collections_view_t const graph_view = {&graph, 0};
    ustore_graph_write_edges_t write_edges = {.database = database,
                                              .outcome = &outcome,
                                              .count = 2,
                                              .collections = graph_view,
                                              .sources = {keys, 0},
                                              .targets = {keys + 1, sizeof(ustore_key_t)}};
    ustore_graph_write_edges(&write_edges);
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_vertex_role_t const role = ustore_vertex_role_any_k;
    ustore_vertex_degree_t const* degrees = NULL;
    ustore_graph_read_edges_t read_edges = {.database = database,
                                            .outcome = &outcome,
                                            .count = 3,
                                            .collections = graph_view,
                                            .vertices = keys_view,
                                            .roles = {&role, 0},
                                            .degrees_per_vertex = &degrees,
                                            .arena = arena};
    ustore_graph_read_edges(&read_edges);
    expect_success_at(&outcome, __FILE__, __LINE__);
    expect_(degrees && degrees[0] == 2 && degrees[1] == 1 && degrees[2] == 1);

    // Documents take their field names as a tape and gather one typed column per field
    ustore_collection_id_t docs = {{{0}}};
    create.name = bytes_of("docs");
    create.modality = ustore_modality_docs_k;
    create.collection = &docs;
    ustore_collection_create(&create);
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_key_t const document_key = key_from(9);
    char const* const documents[1] = {"{\"year\":1815,\"name\":\"ada\"}"};
    ustore_docs_write_t write_documents = {.database = database,
                                           .outcome = &outcome,
                                           .count = 1,
                                           .collections = {&docs, 0},
                                           .keys = {&document_key, 0},
                                           .values = accessor_of(documents)};
    ustore_docs_write(&write_documents);
    expect_success_at(&outcome, __FILE__, __LINE__);
    char const fields_joined[] = "yearname";
    uint32_t const fields_offsets[3] = {0, 4, 8};
    ustore_docs_field_type_t const types[2] = {ustore_docs_field_type_u64_k, ustore_docs_field_type_binary_k};
    ustore_docs_column_view_t const* columns = NULL;
    ustore_docs_gather_t gather = {.database = database,
                                   .outcome = &outcome,
                                   .count = 1,
                                   .collections = {&docs, 0},
                                   .keys = {&document_key, 0},
                                   .fields_count = 2,
                                   .fields = u32_tape_of(fields_joined, fields_offsets),
                                   .columns = &columns,
                                   .arena = arena,
                                   .outputs_types = types};
    ustore_docs_gather(&gather);
    expect_success_at(&outcome, __FILE__, __LINE__);
    uint64_t year = 0;
    if (columns && columns[0].scalars) memcpy(&year, columns[0].scalars, sizeof(year));
    expect_(year == 1815 && tape_holds(&columns[1].strings, 0, "ada"));

    // A page imports from XML, takes an insertion after a Nil anchor, and renders back whole and by the inserted key
    ustore_collection_id_t pages = {{{0}}};
    create.name = bytes_of("pages");
    create.modality = ustore_modality_pages_k;
    create.collection = &pages;
    ustore_collection_create(&create);
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_key_t const page_key = key_from(10);
    char const* const markups[1] = {"<doc><p>one</p></doc>"};
    ustore_pages_import_t import_page = {.database = database,
                                         .outcome = &outcome,
                                         .count = 1,
                                         .collections = {&pages, 0},
                                         .keys = {&page_key, 0},
                                         .values = accessor_of(markups),
                                         .encoding = ustore_page_encoding_xml_k};
    ustore_pages_import(&import_page);
    expect_success_at(&outcome, __FILE__, __LINE__);
    char const* const fragments[1] = {"<h1>title</h1>"};
    ustore_key_t const* landed = NULL;
    ustore_pages_insert_t insert_page = {.database = database,
                                         .outcome = &outcome,
                                         .count = 1,
                                         .collections = {&pages, 0},
                                         .keys = {&page_key, 0},
                                         .values = accessor_of(fragments),
                                         .encoding = ustore_page_encoding_xml_k,
                                         .effective_keys = &landed,
                                         .arena = arena};
    ustore_pages_insert(&insert_page);
    expect_success_at(&outcome, __FILE__, __LINE__);
    expect_(landed != NULL);
    ustore_key_t const inserted_key = landed ? landed[0] : page_key;
    ustore_pages_read_t read_page = {.database = database,
                                     .outcome = &outcome,
                                     .count = 1,
                                     .collections = {&pages, 0},
                                     .keys = {&page_key, 0},
                                     .values = &values,
                                     .arena = arena,
                                     .outputs_encoding = ustore_page_encoding_xml_k};
    ustore_pages_read(&read_page);
    expect_success_at(&outcome, __FILE__, __LINE__);
    expect_(tape_holds(&values, 0, "<h1>title</h1><doc><p>one</p></doc>"));
    read_page.elements = (ustore_keys_view_t) {&inserted_key, 0};
    ustore_pages_read(&read_page);
    expect_success_at(&outcome, __FILE__, __LINE__);
    expect_(tape_holds(&values, 0, "<h1>title</h1>"));

    // A description counts the keys, which a compaction keeps and a clear of the pairs removes
    ustore_database_description_t description = {0};
    ustore_database_describe_t describe = {.database = database, .outcome = &outcome, .description = &description};
    ustore_database_describe(&describe);
    expect_success_at(&outcome, __FILE__, __LINE__);
    expect_(description.engine == ustore_engine_ram_k && description.keys != 0);
    ustore_database_compact_t compact = {.database = database, .outcome = &outcome};
    ustore_database_compact(&compact);
    expect_success_at(&outcome, __FILE__, __LINE__);

    // A plan opens only with an executor; a cancel leaves it live, it continues to its end, reading
    // done, and its finish frees it
    ustore_executor_t compactor = NULL;
    ustore_executor_for_ram_t spawn_compactor = {.executor = &compactor, .outcome = &outcome, .threads = 2};
    ustore_executor_for_ram(&spawn_compactor);
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_database_compact_plan_t plan = NULL;
    ustore_phase_progress_t phase = {.phase = ustore_phase_none_k};
    size_t reports = 0;
    ustore_database_compact_t planned = {.database = database,
                                         .outcome = &outcome,
                                         .progress = &cancel_at_first,
                                         .progress_state = &reports,
                                         .plan = &plan,
                                         .phase = &phase};
    ustore_database_compact(&planned);
    expect_(outcome.status == ustore_status_invalid_argument_k && plan == NULL && reports == 0);
    planned.executor = compactor;
    ustore_database_compact(&planned);
    expect_(outcome.status == ustore_status_cancelled_k && plan != NULL && reports == 1);
    expect_(phase.phase != ustore_phase_none_k && phase.completed <= phase.total);
    planned.progress = NULL;
    ustore_database_compact(&planned);
    expect_success_at(&outcome, __FILE__, __LINE__);
    expect_(phase.phase == ustore_phase_done_k);
    ustore_database_compact_plan_finish_t finish_plan = {.database = database,
                                                         .executor = compactor,
                                                         .outcome = &outcome,
                                                         .plan = plan};
    ustore_database_compact_plan_finish(&finish_plan);
    expect_success_at(&outcome, __FILE__, __LINE__);
    finish_plan.plan = NULL;
    ustore_database_compact_plan_finish(&finish_plan);
    expect_(outcome.status == ustore_status_invalid_argument_k);
    ustore_executor_free(compactor);
    ustore_database_clear_t clear = {.database = database, .outcome = &outcome, .mode = ustore_drop_pairs_k};
    ustore_database_clear(&clear);
    expect_success_at(&outcome, __FILE__, __LINE__);
    ustore_database_describe(&describe);
    expect_success_at(&outcome, __FILE__, __LINE__);
    expect_(description.keys == 0);

    // Names spell every closed set and read back
    ustore_bytes_view_t const graph_name = bytes_of(ustore_modality_name(ustore_modality_graph_k));
    ustore_modality_t modality = 0;
    expect_(ustore_modality_from_name(graph_name, &modality) == ustore_status_success_k);
    expect_(modality == ustore_modality_graph_k);
    expect_(ustore_modality_from_name(bytes_of("bogus"), &modality) == ustore_status_invalid_argument_k);
    ustore_phase_t read_phase = ustore_phase_none_k;
    expect_(ustore_phase_from_name(bytes_of(ustore_phase_name(ustore_phase_reclaim_k)), &read_phase) ==
            ustore_status_success_k);
    expect_(read_phase == ustore_phase_reclaim_k);
    expect_(strcmp(ustore_phase_name((ustore_phase_t)99), "unrecognized") == 0);
    ustore_gpu_vendor_t read_vendor = ustore_gpu_vendor_unknown_k;
    expect_(strcmp(ustore_gpu_vendor_name(ustore_gpu_vendor_apple_k), "apple") == 0);
    expect_(ustore_gpu_vendor_from_name(bytes_of("apple"), &read_vendor) == ustore_status_success_k &&
            read_vendor == ustore_gpu_vendor_apple_k);
    ustore_gpu_class_t const apple_classes[] = {ustore_gpu_class_apple_metal7_k, ustore_gpu_class_apple_metal9_k,
                                                ustore_gpu_class_apple_metal10_k};
    char const* const apple_class_names[] = {"apple-metal7", "apple-metal9", "apple-metal10"};
    for (int index = 0; index != 3; ++index) {
        ustore_gpu_class_t read_class = ustore_gpu_class_unknown_k;
        expect_(strcmp(ustore_gpu_class_name(apple_classes[index]), apple_class_names[index]) == 0);
        expect_(ustore_gpu_class_from_name(bytes_of(apple_class_names[index]), &read_class) ==
                    ustore_status_success_k &&
                read_class == apple_classes[index]);
    }
    ustore_gpu_class_t kept_class = ustore_gpu_class_apple_metal9_k;
    expect_(ustore_gpu_class_from_name(bytes_of("apple-metal8"), &kept_class) == ustore_status_invalid_argument_k &&
            kept_class == ustore_gpu_class_apple_metal9_k);

    ustore_arena_free(arena);
    ustore_database_free(database);
}

int main(int argc, char** argv) {
    start_test_run(argc, argv);
    start_test_case("test_readme_quickstart");
    test_readme_quickstart();
    finish_test_case(NULL);
    start_test_case("test_every_argument_shape_lands");
    test_every_argument_shape_lands();
    finish_test_case(NULL);
    return finish_test_run();
}

#undef expect_
