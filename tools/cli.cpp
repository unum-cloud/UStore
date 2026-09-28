/**
 *  @file tools/cli.cpp
 *  @author Ash Vardanian
 *  @date June 5, 2023
 *  @brief Command-line tool over collections, snapshots and document datasets, one command per invocation.
 */

#include <cstddef> // `std::size_t`
#include <cstdio>  // `stderr`
#include <cstring> // `std::memcpy`, `std::strlen`

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

#include <stringzilla/stringzilla.hpp> // `sz::string_t`

#include <ustore/db.h>      // `ustore_database_init_t`, `ustore_error_t`
#include <ustore/strings.h> // `ustore_strings_read`, `ustore_strings_scan`
#include <ustore/types.hpp> // `sz`

#include "arguments.hpp" // `parse_arguments`, `argument_status_t`
#include "dataset.h"     // `ustore_docs_import`, `ustore_docs_export`

namespace unum::ustore {

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
  snapshot create
  snapshot export <path>
  snapshot drop <id>
  snapshot list

Files end in .ndjson or .csv, and a command without --collection acts on the main collection.)";

/** Names one listing reads per round, which paginates a walk of however many collections exist. */
constexpr ustore_length_t listing_page_k = 1024;

#pragma region Parsing

/** Every command the tool runs. */
enum class command_t {
    none_k,
    collection_create_k,
    collection_drop_k,
    collection_list_k,
    collection_import_k,
    collection_export_k,
    snapshot_create_k,
    snapshot_export_k,
    snapshot_drop_k,
    snapshot_list_k,
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

    /** Snapshot a drop removes. */
    ustore_snapshot_t snapshot = 0;
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

    if (object == "snapshot") {
        if (action == "export") {
            if (argument.empty()) return "The snapshot export path is missing";
            line.snapshot_path = argument;
            line.command = command_t::snapshot_export_k;
            return nullptr;
        }
        if (action == "drop") {
            count_t snapshot;
            if (parse_count(argument, snapshot) != argument_status_t::parsed_k) return "The snapshot id is missing";
            line.snapshot = snapshot.value;
            line.command = command_t::snapshot_drop_k;
            return nullptr;
        }
        if (!argument.empty()) return "Too many arguments";
        if (action == "create") {
            line.command = command_t::snapshot_create_k;
            return nullptr;
        }
        if (action == "list") {
            line.command = command_t::snapshot_list_k;
            return nullptr;
        }
        return "Invalid snapshot action";
    }
    return object.empty() ? "The command is missing" : "Unknown command";
}

#pragma endregion Parsing

#pragma region Commands

/** Prints a red failure line with the database error. */
void print_failure(char const* what, ustore_error_t const& error) noexcept {
    fmt::println(style_of(tone_t::failure_k), "{}: {}", what, error.message);
}

/** Resolves @p name to a collection id, the main collection for an empty name. */
void find_collection(ustore_database_t db, std::string_view name, ustore_collection_t& id,
                     ustore_error_t* error) noexcept {
    if (name.empty()) {
        id = ustore_collection_main_k;
        return;
    }
    ustore_arena_t arena = nullptr;
    ustore_collection_t const names = ustore_collection_names_k;
    ustore_str_view_t const text = name.data();
    auto const text_length = static_cast<ustore_length_t>(name.size());
    ustore_length_t* offsets = nullptr;
    ustore_length_t* lengths = nullptr;
    ustore_byte_t* values = nullptr;
    ustore_strings_read_t read {};
    read.db = db;
    read.error = error;
    read.arena = &arena;
    read.tasks_count = 1;
    read.collections = &names;
    read.strings = &text;
    read.strings_lengths = &text_length;
    read.offsets = &offsets;
    read.lengths = &lengths;
    read.values = &values;
    ustore_strings_read(&read);
    if (!error->status) {
        if (lengths[0] != sizeof(ustore_collection_t)) *error = {ustore_error_not_found_k, "No such collection"};
        else std::memcpy(&id, values + offsets[0], sizeof(id));
    }
    ustore_arena_free(arena);
}

/** Records that @p name addresses @p collection, or removes the name when no collection is given. */
void name_collection(ustore_database_t db, std::string_view name, ustore_collection_t const* collection,
                     ustore_error_t* error) noexcept {
    ustore_arena_t arena = nullptr;
    ustore_collection_t const names = ustore_collection_names_k;
    ustore_str_view_t const text = name.data();
    auto const text_length = static_cast<ustore_length_t>(name.size());
    auto const value = reinterpret_cast<ustore_bytes_cptr_t>(collection);
    ustore_length_t const value_length = sizeof(ustore_collection_t);
    ustore_strings_write_t write {};
    write.db = db;
    write.error = error;
    write.arena = &arena;
    write.tasks_count = 1;
    write.collections = &names;
    write.strings = &text;
    write.strings_lengths = &text_length;
    write.values = collection ? &value : nullptr;
    write.values_lengths = collection ? &value_length : nullptr;
    ustore_strings_write(&write);
    ustore_arena_free(arena);
}

/** Creates a collection and records @p name for it. */
[[nodiscard]] bool create_collection(ustore_database_t db, std::string_view name) noexcept {
    ustore_error_t error {};
    ustore_collection_t id = 0;
    ustore_collection_create_t create {};
    create.db = db;
    create.error = &error;
    create.config = "";
    create.id = &id;
    ustore_collection_create(&create);
    if (!error.status) name_collection(db, name, &id, &error);
    if (error.status) {
        print_failure("Failed to create the collection", error);
        return false;
    }
    fmt::println(style_of(tone_t::success_k), "Collection '{}' created", name);
    return true;
}

/** Drops the collection @p name addresses and forgets the name. */
[[nodiscard]] bool drop_collection(ustore_database_t db, std::string_view name) noexcept {
    ustore_error_t error {};
    ustore_collection_t id = 0;
    find_collection(db, name, id, &error);
    if (!error.status) {
        ustore_collection_drop_t drop {};
        drop.db = db;
        drop.error = &error;
        drop.id = id;
        drop.mode = ustore_drop_keys_vals_handle_k;
        ustore_collection_drop(&drop);
    }
    if (!error.status) name_collection(db, name, nullptr, &error);
    if (error.status) {
        print_failure("Failed to drop the collection", error);
        return false;
    }
    fmt::println(style_of(tone_t::success_k), "Collection '{}' dropped", name);
    return true;
}

/** Prints every collection name, paging the strings modality the names live in. */
[[nodiscard]] bool list_collections(ustore_database_t db) noexcept {
    ustore_error_t error {};
    ustore_arena_t arena = nullptr;
    ustore_collection_t const names = ustore_collection_names_k;
    ustore_str_view_t const prefix = "";
    ustore_length_t const prefix_length = 0;
    ustore_length_t const page = listing_page_k;
    sz::string_t resume;
    while (true) {
        ustore_str_view_t const start = resume.c_str();
        auto const start_length = static_cast<ustore_length_t>(resume.size());
        ustore_length_t* counts = nullptr;
        ustore_length_t* offsets = nullptr;
        ustore_char_t* strings = nullptr;
        ustore_strings_scan_t scan {};
        scan.db = db;
        scan.error = &error;
        scan.arena = &arena;
        scan.tasks_count = 1;
        scan.collections = &names;
        scan.prefixes = &prefix;
        scan.prefixes_lengths = &prefix_length;
        scan.count_limits = &page;
        scan.counts = &counts;
        scan.strings_offsets = &offsets;
        scan.strings = &strings;
        if (!resume.empty()) {
            scan.start_strings = &start;
            scan.start_strings_lengths = &start_length;
        }
        ustore_strings_scan(&scan);
        if (error.status) break;

        std::size_t const found = counts[0];
        for (std::size_t index = 0; index != found; ++index)
            fmt::println(style_of(tone_t::listing_k), "{}",
                         std::string_view {strings + offsets[index], offsets[index + 1] - offsets[index]});
        if (found != page) break;

        // A zero byte appended to the last name is the smallest name above it, so no name repeats
        std::string_view const last {strings + offsets[found - 1], offsets[found] - offsets[found - 1]};
        resume.clear();
        if (!resume.try_append(last.data(), last.size()) || !resume.try_push_back('\0')) {
            error = {ustore_error_out_of_memory_k, "Failed to page the listing"};
            break;
        }
    }
    ustore_arena_free(arena);
    if (error.status) {
        print_failure("Failed to list the collections", error);
        return false;
    }
    return true;
}

/** Imports the documents of the file @p line names into the collection it names. */
[[nodiscard]] bool import_documents(ustore_database_t db, command_line_t const& line) noexcept {
    ustore_error_t error {};
    ustore_collection_t collection = 0;
    find_collection(db, line.collection, collection, &error);
    if (!error.status) {
        ustore_docs_import_t importing {};
        importing.db = db;
        importing.error = &error;
        importing.collection = collection;
        importing.path = line.input.data();
        importing.max_batch_size = line.memory_limit.value;
        importing.id_field = line.id_field.data();
        ustore_docs_import(&importing);
    }
    if (error.status) {
        print_failure("Failed to import", error);
        return false;
    }
    fmt::println(style_of(tone_t::success_k), "Successfully imported");
    return true;
}

/** Exports the collection @p line names into the file it names. */
[[nodiscard]] bool export_documents(ustore_database_t db, command_line_t const& line) noexcept {
    ustore_error_t error {};
    ustore_collection_t collection = 0;
    find_collection(db, line.collection, collection, &error);
    if (!error.status) {
        ustore_docs_export_t exporting {};
        exporting.db = db;
        exporting.error = &error;
        exporting.collection = collection;
        exporting.path = line.output.data();
        exporting.max_batch_size = line.memory_limit.value;
        ustore_docs_export(&exporting);
    }
    if (error.status) {
        print_failure("Failed to export", error);
        return false;
    }
    fmt::println(style_of(tone_t::success_k), "Successfully exported");
    return true;
}

/** Takes a snapshot and prints its identifier. */
[[nodiscard]] bool create_snapshot(ustore_database_t db) noexcept {
    ustore_error_t error {};
    ustore_snapshot_t id = 0;
    ustore_snapshot_create_t create {};
    create.db = db;
    create.error = &error;
    create.id = &id;
    ustore_snapshot_create(&create);
    if (error.status) {
        print_failure("Failed to create a snapshot", error);
        return false;
    }
    fmt::println(style_of(tone_t::success_k), "Snapshot {} created", id);
    return true;
}

/** Writes the newest snapshot to @p path. */
[[nodiscard]] bool export_snapshot(ustore_database_t db, std::string_view path) noexcept {
    ustore_error_t error {};
    ustore_snapshot_export_t exporting {};
    exporting.db = db;
    exporting.error = &error;
    exporting.path = path.data();
    ustore_snapshot_export(&exporting);
    if (error.status) {
        print_failure("Failed to export the snapshot", error);
        return false;
    }
    fmt::println(style_of(tone_t::success_k), "Snapshot exported");
    return true;
}

/** Removes the snapshot @p id names. */
[[nodiscard]] bool drop_snapshot(ustore_database_t db, ustore_snapshot_t id) noexcept {
    ustore_error_t error {};
    ustore_snapshot_drop_t drop {};
    drop.db = db;
    drop.error = &error;
    drop.id = id;
    ustore_snapshot_drop(&drop);
    if (error.status) {
        print_failure("Failed to drop the snapshot", error);
        return false;
    }
    fmt::println(style_of(tone_t::success_k), "Snapshot dropped");
    return true;
}

/** Prints the identifier of every live snapshot. */
[[nodiscard]] bool list_snapshots(ustore_database_t db) noexcept {
    ustore_error_t error {};
    ustore_arena_t arena = nullptr;
    ustore_size_t count = 0;
    ustore_snapshot_t* ids = nullptr;
    ustore_snapshot_list_t list {};
    list.db = db;
    list.error = &error;
    list.arena = &arena;
    list.count = &count;
    list.ids = &ids;
    ustore_snapshot_list(&list);
    if (error.status) {
        print_failure("Failed to list the snapshots", error);
        ustore_arena_free(arena);
        return false;
    }
    for (std::size_t index = 0; index != count; ++index) fmt::println(style_of(tone_t::listing_k), "{}", ids[index]);
    ustore_arena_free(arena);
    return true;
}

/** Runs the command of @p line and answers whether it succeeded. */
[[nodiscard]] bool execute(ustore_database_t db, command_line_t const& line) noexcept {
    switch (line.command) {
    case command_t::none_k: return false;
    case command_t::collection_create_k: return create_collection(db, line.collection);
    case command_t::collection_drop_k: return drop_collection(db, line.collection);
    case command_t::collection_list_k: return list_collections(db);
    case command_t::collection_import_k: return import_documents(db, line);
    case command_t::collection_export_k: return export_documents(db, line);
    case command_t::snapshot_create_k: return create_snapshot(db);
    case command_t::snapshot_export_k: return export_snapshot(db, line.snapshot_path);
    case command_t::snapshot_drop_k: return drop_snapshot(db, line.snapshot);
    case command_t::snapshot_list_k: return list_snapshots(db);
    }
    return false;
}

#pragma endregion Commands

} // namespace unum::ustore

int main(int argc, char* argv[]) {
    using namespace unum::ustore;

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
        {"--memory-limit", "size", "bytes an import or export buffers per batch, like 64MiB", &line.memory_limit},
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
        fmt::println(stderr, "{}: {}", parsed.argument, describe(parsed.status));
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

    ustore_database_t db = nullptr;
    ustore_error_t error {};
    line.engine.directory = line.directory.data();
    open_engine(line.engine, &db, &error);
    if (error.status) {
        print_failure("Failed to open the database", error);
        return 1;
    }

    bool const succeeded = execute(db, line);
    ustore_database_free(db);
    return succeeded ? 0 : 1;
}
