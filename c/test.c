/**
 *  @file c/test.c
 *  @author Ash Vardanian
 *  @date October 13, 2026
 *  @brief Calls the C ABI from plain C11 once per shape of its arguments, so a header that only a C++ compiler accepts
 *      fails to build, and a shape the library misreads fails the run.
 */
#include <stdio.h>  // `fprintf`, `stderr`
#include <string.h> // `memcmp`, `memset`, `strlen`

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

    // No collection exists until one is created, and every call names the collections it addresses
    ustore_collection_id_t blobs = {{{0}}};
    ustore_collection_create_t create_blobs = {.database = database,
                                               .outcome = &outcome,
                                               .name = bytes_of("values"),
                                               .modality = ustore_modality_blobs_k,
                                               .collection = &blobs};
    ustore_collection_create(&create_blobs);
    failures += expect_success(&outcome, __LINE__);
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
                                .collections = blobs_view,
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
                                .collections = blobs_view,
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
    failures += expect(listed == 2 && (tape_holds(&names, 0, "colors") || tape_holds(&names, 1, "colors")), __LINE__);

    // Strings read through an accessor map onto strided keys, and read back through a tape of the same strings
    ustore_collections_view_t const colors_view = {&colors, 0};
    ustore_strings_write_t write_strings = {.database = database,
                                            .outcome = &outcome,
                                            .count = 3,
                                            .collections = colors_view,
                                            .strings = accessor_of(texts),
                                            .values = keys_view};
    ustore_strings_write(&write_strings);
    failures += expect_success(&outcome, __LINE__);
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
    failures += expect_success(&outcome, __LINE__);
    failures += expect(presences && presences[0] == 7 && color_keys, __LINE__);
    failures += expect(color_keys && memcmp(color_keys, keys, sizeof(keys)) == 0, __LINE__);

    // A cursor pulls the keys of a collection with their values, then nothing once a page came back short
    ustore_blobs_cursor_t keys_cursor = NULL;
    ustore_blobs_cursor_init_t keys_cursor_init = {.cursor = &keys_cursor,
                                                   .database = database,
                                                   .outcome = &outcome,
                                                   .collection = blobs};
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

    // A tape a read wrote exports as an Arrow column pinning its arena, which refuses the next call until released
    ustore_arena_t exported = NULL;
    ustore_arena_init_t exported_init = {.arena = &exported, .outcome = &outcome};
    ustore_arena_init(&exported_init);
    read.arena = exported;
    ustore_blobs_read(&read);
    failures += expect_success(&outcome, __LINE__);
    struct ArrowSchema schema = {0};
    struct ArrowDeviceArray column = {0};
    failures += expect(ustore_strings_view_to_arrow(values, 3, ustore_strings_u32_tape_k, exported, &schema, &column) ==
                           ustore_status_success_k,
                       __LINE__);
    ustore_blobs_read(&read);
    failures += expect(outcome.status == ustore_status_contended_k, __LINE__);
    ustore_strings_view_t converted = {0};
    ustore_size_t converted_count = 0;
    failures += expect(ustore_strings_view_from_arrow(&schema, &column.array, &converted, &converted_count) ==
                               ustore_status_success_k &&
                           converted_count == 3 && tape_holds(&converted, 2, "blue"),
                       __LINE__);
    ustore_arena_free(exported);
    if (column.array.release) column.array.release(&column.array);
    if (schema.release) schema.release(&schema);
    read.arena = arena;

    // A strings cursor copies its prefix and walks the strings it starts, with the keys they map onto
    ustore_strings_cursor_t strings_cursor = NULL;
    ustore_strings_cursor_init_t strings_cursor_init = {.cursor = &strings_cursor,
                                                        .database = database,
                                                        .outcome = &outcome,
                                                        .collection = colors,
                                                        .prefix = bytes_of("gr")};
    ustore_strings_cursor_init(&strings_cursor_init);
    failures += expect_success(&outcome, __LINE__);
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
    failures += expect_success(&outcome, __LINE__);
    failures += expect(pulled == 1 && tape_holds(&walked, 0, "green"), __LINE__);
    failures += expect(walked_keys && memcmp(walked_keys, &keys[1], sizeof(ustore_key_t)) == 0, __LINE__);
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
    ustore_collection_id_t docs = {{{0}}};
    create.name = bytes_of("docs");
    create.modality = ustore_modality_docs_k;
    create.collection = &docs;
    ustore_collection_create(&create);
    failures += expect_success(&outcome, __LINE__);
    ustore_key_t const document_key = key_from(9);
    char const* const documents[1] = {"{\"year\":1815,\"name\":\"ada\"}"};
    ustore_docs_write_t write_documents = {.database = database,
                                           .outcome = &outcome,
                                           .count = 1,
                                           .collections = {&docs, 0},
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
                                   .collections = {&docs, 0},
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

    // A page imports from XML, takes an insertion after a Nil anchor, and renders back whole and by the inserted key
    ustore_collection_id_t pages = {{{0}}};
    create.name = bytes_of("pages");
    create.modality = ustore_modality_pages_k;
    create.collection = &pages;
    ustore_collection_create(&create);
    failures += expect_success(&outcome, __LINE__);
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
    failures += expect_success(&outcome, __LINE__);
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
    failures += expect_success(&outcome, __LINE__);
    failures += expect(landed != NULL, __LINE__);
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
    failures += expect_success(&outcome, __LINE__);
    failures += expect(tape_holds(&values, 0, "<h1>title</h1><doc><p>one</p></doc>"), __LINE__);
    read_page.elements = (ustore_keys_view_t) {&inserted_key, 0};
    ustore_pages_read(&read_page);
    failures += expect_success(&outcome, __LINE__);
    failures += expect(tape_holds(&values, 0, "<h1>title</h1>"), __LINE__);

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

    // Names spell every closed set and read back
    ustore_bytes_view_t const graph_name = bytes_of(ustore_modality_name(ustore_modality_graph_k));
    failures += expect(ustore_modality_from_name(graph_name) == ustore_modality_graph_k, __LINE__);

    ustore_arena_free(arena);
    ustore_database_free(database);
    if (failures) fprintf(stderr, "%zu checks failed\n", failures);
    return failures ? 1 : 0;
}
