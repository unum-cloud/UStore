/**
 *  @file c/test.c
 *  @author Ash Vardanian
 *  @date October 13, 2026
 *  @brief Calls the C ABI from plain C11 once per shape of its arguments, so a header that only a C++ compiler accepts
 *      fails to build, and a shape the library misreads fails the run.
 */
#include <stdio.h>  // `fprintf`, `snprintf`, `stderr`
#include <string.h> // `memcmp`, `memset`, `strcmp`, `strlen`

#include <stddef.h> // `size_t`, `NULL`
#include <stdint.h> // `uint32_t`, `uint64_t`

#include "ustore/ustore.h"

/** Counts one failure, naming @p line, unless @p holds. */
static size_t expect(int holds, int line) {
    if (holds) return 0;
    fprintf(stderr, "check failed at c/test.c:%d\n", line);
    return 1;
}

/** Counts one failure, naming @p line and the message, unless @p outcome reports success. */
static size_t expect_success(ustore_outcome_t const* outcome, int line) {
    if (outcome->status == ustore_status_success_k) return 0;
    fprintf(stderr, "call failed at c/test.c:%d: %s\n", line, outcome->message ? outcome->message : "no message");
    return 1;
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

int main(void) {
    size_t failures = 0;
    ustore_outcome_t outcome = {0};

    // The in-memory engine starts empty and persists nowhere
    ustore_database_t database = NULL;
    ustore_database_init_ram_t init = {.database = &database, .outcome = &outcome};
    ustore_database_init_ram(&init);
    failures += expect_success(&outcome, __LINE__);
    ustore_arena_t arena = NULL;
    ustore_arena_init_t arena_init = {.arena = &arena, .outcome = &outcome};
    ustore_arena_init(&arena_init);
    failures += expect_success(&outcome, __LINE__);
    if (failures) return 1;

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
                                  .keys = keys_view,
                                  .values = narrow_tape};
    ustore_blobs_write(&write);
    failures += expect_success(&outcome, __LINE__);
    write.values = u64_tape_of(joined, wide_offsets);
    ustore_blobs_write(&write);
    failures += expect_success(&outcome, __LINE__);
    write.values = accessor_of(texts);
    ustore_blobs_write(&write);
    failures += expect_success(&outcome, __LINE__);

    // A read answers presences, lengths and a tape of values in the arena
    ustore_octet_t const* presences = NULL;
    ustore_length_t const* lengths = NULL;
    ustore_strings_view_t values = {0};
    ustore_blobs_read_t read = {.database = database,
                                .outcome = &outcome,
                                .count = 3,
                                .keys = keys_view,
                                .presences = &presences,
                                .lengths = &lengths,
                                .values = &values,
                                .arena = arena};
    ustore_blobs_read(&read);
    failures += expect_success(&outcome, __LINE__);
    failures += expect(presences && presences[0] == 7 && lengths && lengths[1] == 5, __LINE__);
    failures += expect(tape_holds(&values, 0, "red") && tape_holds(&values, 2, "blue"), __LINE__);

    // A scan repeats one start key for its single task through a zero stride
    ustore_length_t const* scan_offsets = NULL;
    ustore_key_t const* found_keys = NULL;
    ustore_blobs_scan_t scan = {.database = database,
                                .outcome = &outcome,
                                .count = 1,
                                .start_keys = {keys, 0},
                                .offsets = &scan_offsets,
                                .keys = &found_keys,
                                .arena = arena,
                                .wanted = 3};
    ustore_blobs_scan(&scan);
    failures += expect_success(&outcome, __LINE__);
    failures += expect(scan_offsets && found_keys && scan_offsets[1] == 3, __LINE__);

    // A collection takes its name as bytes, and a listing answers the names as a tape
    ustore_collection_id_t colors = {{{0}}};
    ustore_collection_create_t create = {.database = database,
                                         .outcome = &outcome,
                                         .name = bytes_of("colors"),
                                         .modality = ustore_modality_strings_k,
                                         .collection = &colors};
    ustore_collection_create(&create);
    failures += expect_success(&outcome, __LINE__);
    ustore_size_t listed = 0;
    ustore_strings_view_t names = {0};
    ustore_collection_list_t list = {.database = database,
                                     .outcome = &outcome,
                                     .names = &names,
                                     .written = &listed,
                                     .arena = arena};
    ustore_collection_list(&list);
    failures += expect_success(&outcome, __LINE__);
    failures += expect(listed == 1 && tape_holds(&names, 0, "colors"), __LINE__);

    // Strings keyed by an accessor take values from a tape, and read back through a tape of the same strings
    ustore_collections_view_t const colors_view = {&colors, 0};
    ustore_strings_write_t write_strings = {.database = database,
                                            .outcome = &outcome,
                                            .count = 3,
                                            .collections = colors_view,
                                            .strings = accessor_of(texts),
                                            .values = narrow_tape};
    ustore_strings_write(&write_strings);
    failures += expect_success(&outcome, __LINE__);
    ustore_strings_read_t read_strings = {.database = database,
                                          .outcome = &outcome,
                                          .count = 3,
                                          .collections = colors_view,
                                          .strings = narrow_tape,
                                          .presences = &presences,
                                          .values = &values,
                                          .arena = arena};
    ustore_strings_read(&read_strings);
    failures += expect_success(&outcome, __LINE__);
    failures += expect(presences && presences[0] == 7 && tape_holds(&values, 1, "green"), __LINE__);

    // A cursor pulls the keys of the main collection with their values, then nothing once a page came back short
    ustore_blobs_cursor_t keys_cursor = NULL;
    ustore_blobs_cursor_init_t keys_cursor_init = {.cursor = &keys_cursor, .database = database, .outcome = &outcome};
    ustore_blobs_cursor_init(&keys_cursor_init);
    failures += expect_success(&outcome, __LINE__);
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
    failures += expect_success(&outcome, __LINE__);
    failures += expect(pulled == 2 && tape_holds(&values, 1, "green"), __LINE__);
    ustore_blobs_cursor_next(&keys_next);
    failures += expect_success(&outcome, __LINE__);
    failures += expect(pulled == 1 && tape_holds(&values, 0, "blue"), __LINE__);
    ustore_blobs_cursor_next(&keys_next);
    failures += expect(pulled == 0 && found_keys == NULL, __LINE__);
    ustore_blobs_cursor_free(keys_cursor);

    // A strings cursor copies its prefix and walks the strings it starts
    ustore_strings_cursor_t strings_cursor = NULL;
    ustore_strings_cursor_init_t strings_cursor_init = {.cursor = &strings_cursor,
                                                        .database = database,
                                                        .outcome = &outcome,
                                                        .collection = colors,
                                                        .prefix = bytes_of("gr")};
    ustore_strings_cursor_init(&strings_cursor_init);
    failures += expect_success(&outcome, __LINE__);
    ustore_strings_view_t walked = {0};
    ustore_strings_cursor_next_t strings_next = {.database = database,
                                                 .outcome = &outcome,
                                                 .cursor = strings_cursor,
                                                 .strings = &walked,
                                                 .written = &pulled,
                                                 .arena = arena,
                                                 .wanted = 8};
    ustore_strings_cursor_next(&strings_next);
    failures += expect_success(&outcome, __LINE__);
    failures += expect(pulled == 1 && tape_holds(&walked, 0, "green"), __LINE__);
    ustore_strings_cursor_free(strings_cursor);

    // An executor serves every memory domain the process may allocate on when its options name none
    ustore_executor_t executor = NULL;
    ustore_executor_for_ram_t spawn = {.executor = &executor, .outcome = &outcome, .ram = {NULL, 0}, .threads = 1};
    ustore_executor_for_ram(&spawn);
    failures += expect_success(&outcome, __LINE__);
    ustore_executor_free(executor);

    // Edges take strided sources and targets, and a read takes one role for every vertex
    ustore_collection_id_t graph = {{{0}}};
    create.name = bytes_of("graph");
    create.modality = ustore_modality_graph_k;
    create.collection = &graph;
    ustore_collection_create(&create);
    failures += expect_success(&outcome, __LINE__);
    ustore_collections_view_t const graph_view = {&graph, 0};
    ustore_graph_write_edges_t write_edges = {.database = database,
                                              .outcome = &outcome,
                                              .count = 2,
                                              .collections = graph_view,
                                              .sources = {keys, 0},
                                              .targets = {keys + 1, sizeof(ustore_key_t)}};
    ustore_graph_write_edges(&write_edges);
    failures += expect_success(&outcome, __LINE__);
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
    failures += expect_success(&outcome, __LINE__);
    failures += expect(degrees && degrees[0] == 2 && degrees[1] == 1 && degrees[2] == 1, __LINE__);

    // Documents take their field names as a tape and gather one typed column per field
    ustore_key_t const document_key = key_from(9);
    char const* const documents[1] = {"{\"year\":1815,\"name\":\"ada\"}"};
    ustore_docs_write_t write_documents = {.database = database,
                                           .outcome = &outcome,
                                           .count = 1,
                                           .keys = {&document_key, 0},
                                           .values = accessor_of(documents)};
    ustore_docs_write(&write_documents);
    failures += expect_success(&outcome, __LINE__);
    char const fields_joined[] = "yearname";
    uint32_t const fields_offsets[3] = {0, 4, 8};
    ustore_docs_field_type_t const types[2] = {ustore_docs_field_type_u64_k, ustore_docs_field_type_bin_k};
    ustore_docs_column_view_t const* columns = NULL;
    ustore_docs_gather_t gather = {.database = database,
                                   .outcome = &outcome,
                                   .count = 1,
                                   .keys = {&document_key, 0},
                                   .fields_count = 2,
                                   .fields = u32_tape_of(fields_joined, fields_offsets),
                                   .columns = &columns,
                                   .arena = arena,
                                   .outputs_types = types};
    ustore_docs_gather(&gather);
    failures += expect_success(&outcome, __LINE__);
    uint64_t year = 0;
    if (columns && columns[0].scalars) memcpy(&year, columns[0].scalars, sizeof(year));
    failures += expect(year == 1815 && tape_holds(&columns[1].strings, 0, "ada"), __LINE__);

    // A description counts the keys, which a compaction keeps and a clear of the pairs removes
    ustore_database_description_t description = {0};
    ustore_database_describe_t describe = {.database = database, .outcome = &outcome, .description = &description};
    ustore_database_describe(&describe);
    failures += expect_success(&outcome, __LINE__);
    failures += expect(description.engine == ustore_engine_ram_k && description.keys != 0, __LINE__);
    ustore_database_compact_t compact = {.database = database, .outcome = &outcome};
    ustore_database_compact(&compact);
    failures += expect_success(&outcome, __LINE__);
    ustore_database_clear_t clear = {.database = database, .outcome = &outcome, .mode = ustore_drop_pairs_k};
    ustore_database_clear(&clear);
    failures += expect_success(&outcome, __LINE__);
    ustore_database_describe(&describe);
    failures += expect_success(&outcome, __LINE__);
    failures += expect(description.keys == 0, __LINE__);

    // Names spell every closed set and read back, and the version spells the macros
    ustore_bytes_view_t const graph_name = bytes_of(ustore_modality_name(ustore_modality_graph_k));
    failures += expect(ustore_modality_from_name(graph_name) == ustore_modality_graph_k, __LINE__);
    char version[32];
    snprintf(version, sizeof(version), "%d.%d.%d", USTORE_VERSION_MAJOR, USTORE_VERSION_MINOR, USTORE_VERSION_PATCH);
    failures += expect(strcmp(version, ustore_version_name()) == 0, __LINE__);

    ustore_arena_free(arena);
    ustore_database_free(database);
    if (failures) fprintf(stderr, "%zu checks failed\n", failures);
    return failures ? 1 : 0;
}
