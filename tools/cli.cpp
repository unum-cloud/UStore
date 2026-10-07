/**
 *  @file tools/cli.cpp
 *  @author Ash Vardanian
 *  @date June 5, 2023
 *  @brief Command-line tool over collections, snapshots and document datasets, one command per invocation.
 */

#include <cstddef> // `std::size_t`
#include <cstdio>  // `stderr`

#if defined(_WIN32)
#include <io.h> // `_isatty`, `_fileno`
#else
#include <unistd.h> // `isatty`
#endif

#include <array>       // `std::array`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`

#include <fmt/color.h>  // `fmt::color`, `fmt::fg`, `fmt::text_style`
#include <fmt/format.h> // `fmt::println`, `fmt::format_to_n`

#include "ustore/db.h"      // `ustore_database_t`, `ustore_outcome_t`
#include "ustore/types.hpp" // `uuid_to_text`, `uuid_text_length_k`

#include "arguments.hpp" // `parse_arguments`, `print_error`
#include "dataset.h"     // `ustore_docs_import`, `ustore_docs_export`

namespace unum::ustore::tools {

/** How a message reads, which decides its color on a terminal. */
enum class tone_t : std::uint8_t {
    failure_k,
    success_k,
    listing_k,
};

#if defined(_WIN32)

/** Whether @c stdout writes to a terminal. */
inline bool stdout_is_terminal() noexcept { return ::_isatty(::_fileno(stdout)) != 0; }
#else

/** Whether @c stdout writes to a terminal. */
inline bool stdout_is_terminal() noexcept { return ::isatty(::fileno(stdout)) != 0; }
#endif

/** The style of a message in @p tone, empty when @c stdout is piped so logs stay plain. */
inline fmt::text_style style_of(tone_t tone) noexcept {
    if (!stdout_is_terminal()) return {};
    switch (tone) {
    case tone_t::failure_k: return fmt::fg(fmt::color::red);
    case tone_t::success_k: return fmt::fg(fmt::color::green);
    case tone_t::listing_k: return fmt::fg(fmt::color::gold);
    }
    return {};
}

/** The command words, printed between the synopsis and the flags. */
constexpr std::string_view commands_k = R"(Commands:
  collection create --collection <name>
  collection drop --collection <name>
  collection list
  collection import --input <file> --id-field <field> --memory-limit <size> [--collection <name>]
  collection export --output <file> --memory-limit <size> [--collection <name>]
  snapshot export <path>

Files end in .ndjson or .csv, and a command without --collection acts on the main collection.)";

/** The spelling of every modality in the order of @c ustore_modality_t, as a listing prints it. */
constexpr std::array<std::string_view, 6> modality_names_k {"any", "blobs", "docs", "graph", "strings", "pages"};

#pragma region Parsing

/** Every command the tool runs. */
enum class command_t {
    none_k,
    collection_create_k,
    collection_drop_k,
    collection_list_k,
    collection_import_k,
    collection_export_k,
    snapshot_export_k,
};

/** One invocation: the flags, which view into @c argv and so end in null terminators, and the command they serve. */
struct command_line_t {

    /** The command to run. */
    command_t command = command_t::none_k;

    /** Database directory. */
    std::string_view directory;

    /** The engine to open and the options of its init call. */
    engine_options_t engine;

    /** Collection name, empty for the main collection. */
    std::string_view collection;

    /** File a document import reads. */
    std::string_view input;

    /** File a document export writes. */
    std::string_view output;

    /** Field holding the keys of imported documents. */
    std::string_view id_field;

    /** Bytes an import or export buffers per batch. */
    bytes_t memory_limit;

    /** Path a snapshot export writes. */
    std::string_view snapshot_path;
};

/** Resolves @p positionals into the command of @p line, answering the problem with them, or null when they parse. */
[[nodiscard]] char const* parse_command(std::span<std::string_view const> positionals, command_line_t& line) noexcept {
    std::string_view const object = positionals.size() > 0 ? positionals[0] : "";
    std::string_view const action = positionals.size() > 1 ? positionals[1] : "";
    std::string_view const argument = positionals.size() > 2 ? positionals[2] : "";

    if (object == "collection") {
        if (!argument.empty()) return "Too many arguments";
        if (action == "create" || action == "drop") {
            if (line.collection.empty()) return "The collection name is missing";
            line.command = action == "create" ? command_t::collection_create_k : command_t::collection_drop_k;
            return nullptr;
        }
        if (action == "list") {
            line.command = command_t::collection_list_k;
            return nullptr;
        }
        if (action == "import") {
            if (line.input.empty() || line.id_field.empty() || line.memory_limit.value == 0)
                return "An import needs --input, --id-field and --memory-limit";
            line.command = command_t::collection_import_k;
            return nullptr;
        }
        if (action == "export") {
            if (line.output.empty() || line.memory_limit.value == 0)
                return "An export needs --output and --memory-limit";
            line.command = command_t::collection_export_k;
            return nullptr;
        }
        return "Invalid collection action";
    }

    // A snapshot lives no longer than the process that took it, so exporting one is the only use a command has
    if (object == "snapshot") {
        if (action != "export") return "Invalid snapshot action";
        if (argument.empty()) return "The snapshot export path is missing";
        line.snapshot_path = argument;
        line.command = command_t::snapshot_export_k;
        return nullptr;
    }
    return object.empty() ? "The command is missing" : "Unknown command";
}

#pragma endregion Parsing

#pragma region Commands

/** Whether @p outcome holds a failure. */
[[nodiscard]] bool failed(ustore_outcome_t const& outcome) noexcept {
    return outcome.status != ustore_status_success_k;
}

/** Prints a red failure line with the database outcome. */
void print_failure(char const* what, ustore_outcome_t const& outcome) noexcept {
    fmt::println(style_of(tone_t::failure_k), "{}: {}", what, outcome.message);
}

/** Resolves @p name to a collection ID, the main collection for an empty name. */
void find_collection(ustore_database_t database, std::string_view name, ustore_collection_id_t& id,
                     ustore_outcome_t* outcome) noexcept {
    if (name.empty()) {
        id = ustore_collection_main_k;
        return;
    }
    ustore_str_view_t const text = name.data();
    auto const text_length = static_cast<ustore_length_t>(name.size());
    ustore_octet_t presence = 0;
    ustore_collection_find_t find {};
    find.database = database;
    find.outcome = outcome;
    find.count = 1;
    find.names = &text;
    find.names_lengths = &text_length;
    find.presences = &presence;
    find.collections = &id;
    find.collections_stride = sizeof(ustore_collection_id_t);
    ustore_collection_find(&find);
    if (!failed(*outcome) && !(presence & 1)) *outcome = {ustore_status_not_found_k, "No such collection"};
}

/** Creates a collection under @p name. */
[[nodiscard]] bool create_collection(ustore_database_t database, std::string_view name) noexcept {
    ustore_outcome_t outcome {};
    ustore_collection_id_t id {};
    ustore_collection_create_t create {};
    create.database = database;
    create.outcome = &outcome;
    create.name = name.data();
    create.name_length = static_cast<ustore_length_t>(name.size());
    create.collection = &id;
    ustore_collection_create(&create);
    if (failed(outcome)) {
        print_failure("Failed to create the collection", outcome);
        return false;
    }
    fmt::println(style_of(tone_t::success_k), "Collection '{}' created", name);
    return true;
}

/** Drops the collection @p name addresses, which takes the name with it. */
[[nodiscard]] bool drop_collection(ustore_database_t database, std::string_view name) noexcept {
    ustore_outcome_t outcome {};
    ustore_collection_drop_t drop {};
    drop.database = database;
    drop.outcome = &outcome;
    drop.mode = ustore_drop_keys_vals_handle_k;
    if (name.empty()) outcome = {ustore_status_invalid_argument_k, "The main collection is never dropped"};
    else find_collection(database, name, drop.collection, &outcome);
    if (!failed(outcome)) ustore_collection_drop(&drop);
    if (failed(outcome)) {
        print_failure("Failed to drop the collection", outcome);
        return false;
    }
    fmt::println(style_of(tone_t::success_k), "Collection '{}' dropped", name);
    return true;
}

/** Prints every collection with its identifier, its name and what it holds. */
[[nodiscard]] bool list_collections(ustore_database_t database) noexcept {
    ustore_outcome_t outcome {};
    ustore_arena_t arena = nullptr;
    ustore_size_t count = 0;
    ustore_collection_id_t* ids = nullptr;
    ustore_modality_t* modalities = nullptr;
    ustore_length_t* names_offsets = nullptr;
    ustore_char_t* names = nullptr;
    ustore_collection_list_t list {};
    list.database = database;
    list.outcome = &outcome;
    list.arena = &arena;
    list.count = &count;
    list.collections = &ids;
    list.modalities = &modalities;
    list.names_offsets = &names_offsets;
    list.names = &names;
    ustore_collection_list(&list);
    for (std::size_t index = 0; !failed(outcome) && index != count; ++index) {
        std::string_view const name {names + names_offsets[index], names_offsets[index + 1] - names_offsets[index]};
        std::size_t const modality = static_cast<std::size_t>(modalities[index]);
        char id_text[uuid_text_length_k];
        uuid_to_text(ids[index].uuid, id_text);
        fmt::println(style_of(tone_t::listing_k), "{}\t{}\t{}", std::string_view {id_text, uuid_text_length_k}, name,
                     modality < modality_names_k.size() ? modality_names_k[modality] : "unknown");
    }
    ustore_arena_free(arena);
    if (failed(outcome)) {
        print_failure("Failed to list the collections", outcome);
        return false;
    }
    return true;
}

/** Imports the documents of the file @p line names into the collection it names. */
[[nodiscard]] bool import_documents(ustore_database_t database, command_line_t const& line) noexcept {
    ustore_outcome_t outcome {};
    ustore_collection_id_t collection {};
    find_collection(database, line.collection, collection, &outcome);
    if (!failed(outcome)) {
        ustore_docs_import_t importing {};
        importing.database = database;
        importing.outcome = &outcome;
        importing.collection = collection;
        importing.path = line.input.data();
        importing.max_batch_size = line.memory_limit.value;
        importing.id_field = line.id_field.data();
        ustore_docs_import(&importing);
    }
    if (failed(outcome)) {
        print_failure("Failed to import", outcome);
        return false;
    }
    fmt::println(style_of(tone_t::success_k), "Successfully imported");
    return true;
}

/** Exports the collection @p line names into the file it names. */
[[nodiscard]] bool export_documents(ustore_database_t database, command_line_t const& line) noexcept {
    ustore_outcome_t outcome {};
    ustore_collection_id_t collection {};
    find_collection(database, line.collection, collection, &outcome);
    if (!failed(outcome)) {
        ustore_docs_export_t exporting {};
        exporting.database = database;
        exporting.outcome = &outcome;
        exporting.collection = collection;
        exporting.path = line.output.data();
        exporting.max_batch_size = line.memory_limit.value;
        ustore_docs_export(&exporting);
    }
    if (failed(outcome)) {
        print_failure("Failed to export", outcome);
        return false;
    }
    fmt::println(style_of(tone_t::success_k), "Successfully exported");
    return true;
}

/** Takes a snapshot of the head and writes it to @p path. */
[[nodiscard]] bool export_snapshot(ustore_database_t database, std::string_view path) noexcept {
    ustore_outcome_t outcome {};
    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t init {};
    init.database = database;
    init.outcome = &outcome;
    init.snapshot = &snapshot;
    ustore_snapshot_init(&init);
    if (!failed(outcome)) {
        ustore_snapshot_export_t exporting {};
        exporting.database = database;
        exporting.outcome = &outcome;
        exporting.snapshot = snapshot;
        exporting.path = path.data();
        ustore_snapshot_export(&exporting);
    }
    ustore_snapshot_free(snapshot);
    if (failed(outcome)) {
        print_failure("Failed to export the snapshot", outcome);
        return false;
    }
    fmt::println(style_of(tone_t::success_k), "Snapshot exported");
    return true;
}

/** Runs the command of @p line and answers whether it succeeded. */
[[nodiscard]] bool execute(ustore_database_t database, command_line_t const& line) noexcept {
    switch (line.command) {
    case command_t::none_k: return false;
    case command_t::collection_create_k: return create_collection(database, line.collection);
    case command_t::collection_drop_k: return drop_collection(database, line.collection);
    case command_t::collection_list_k: return list_collections(database);
    case command_t::collection_import_k: return import_documents(database, line);
    case command_t::collection_export_k: return export_documents(database, line);
    case command_t::snapshot_export_k: return export_snapshot(database, line.snapshot_path);
    }
    return false;
}

#pragma endregion Commands

} // namespace unum::ustore::tools

int main(int argc, char* argv[]) {
    using namespace unum::ustore;
    using namespace unum::ustore::tools;

    command_line_t line;
    std::array<flag_t, 9> const flags {{
        {"--directory", "path", "database directory, required", &line.directory},
        {"--engine", "name", "engine to open: ram, rocksdb, leveldb or nvme", &line.engine.name},
        {"--isolation", "name", "isolation of the transactions of ram", &line.engine.isolation},
        {"--engine-transport", "name", "transport of nvme", &line.engine.transport},
        {"--collection", "name", "collection a command acts on, the main one when omitted", &line.collection},
        {"--input", "file", "file a collection import reads", &line.input},
        {"--output", "file", "file a collection export writes", &line.output},
        {"--id-field", "field", "field holding the keys of imported documents", &line.id_field},
        {"--memory-limit", "size", "bytes an import or export buffers per batch, like 64MB", &line.memory_limit},
    }};
    std::array<std::string_view, 3> positionals {};
    arguments_t const parsed = parse_arguments(argc, argv, flags, positionals);

    char synopsis[4096 + commands_k.size()];
    auto const written = fmt::format_to_n(synopsis, sizeof(synopsis) - 1,
                                          "{} --directory <path> <command> [flags]\n\n{}", argv[0], commands_k);
    *written.out = '\0';

    if (parsed.status == argument_status_t::help_k) {
        print_usage(synopsis, flags);
        return 0;
    }
    if (parsed.status != argument_status_t::parsed_k) {
        print_error(parsed);
        return 1;
    }
    char const* const problem = line.directory.empty()
                                    ? "The --directory flag is required"
                                    : parse_command({positionals.data(), parsed.positionals_count}, line);
    if (problem) {
        fmt::println(stderr, "{}\n", problem);
        print_usage(synopsis, flags);
        return 1;
    }

    ustore_database_t database = nullptr;
    ustore_outcome_t outcome {};
    line.engine.directory = line.directory.data();
    open_engine(line.engine, &database, &outcome);
    if (failed(outcome)) {
        print_failure("Failed to open the database", outcome);
        return 1;
    }

    bool const succeeded = execute(database, line);
    ustore_database_free(database);
    return succeeded ? 0 : 1;
}
