/**
 *  @file tools/cli.cpp
 *  @author Ash Vardanian
 *  @date June 5, 2023
 *  @brief Command-line tool over collections, snapshots and datasets, one command per invocation.
 *
 *  Status lines go to standard error, so an export to @c - leaves standard output carrying only Arrow bytes.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstdio>  // `stderr`

#if defined(_WIN32)
#include <io.h> // `_isatty`, `_fileno`
#else
#include <unistd.h> // `isatty`
#endif

#include <array>        // `std::array`
#include <filesystem>   // `std::filesystem::directory_iterator`, `std::filesystem::is_empty`
#include <span>         // `std::span`
#include <string_view>  // `std::string_view`
#include <system_error> // `std::error_code`

#include <fmt/color.h>  // `fmt::color`, `fmt::fg`, `fmt::text_style`
#include <fmt/format.h> // `fmt::println`, `fmt::format_to_n`

#include <smashtable/basic_vector.hpp> // `st::vector`
#include <stringzilla/stringzilla.hpp> // `sz::string_t`, `sz::argsort`

#include "ustore/db.h"       // `ustore_database_t`, `ustore_outcome_t`
#include "ustore/ranges.hpp" // `u64_tape`, `tape_string`
#include "ustore/types.hpp"  // `uuid_to_text`, `uuid_text_length_k`, `bytes_of`

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

/** Whether @p stream writes to a terminal. */
inline bool is_terminal(std::FILE* stream) noexcept { return ::_isatty(::_fileno(stream)) != 0; }
#else

/** Whether @p stream writes to a terminal. */
inline bool is_terminal(std::FILE* stream) noexcept { return ::isatty(::fileno(stream)) != 0; }
#endif

/** The style of a message in @p tone printed to @p stream, empty when it is piped so logs stay plain. */
inline fmt::text_style style_of(tone_t tone, std::FILE* stream) noexcept {
    if (!is_terminal(stream)) return {};
    switch (tone) {
    case tone_t::failure_k: return fmt::fg(fmt::color::red);
    case tone_t::success_k: return fmt::fg(fmt::color::green);
    case tone_t::listing_k: return fmt::fg(fmt::color::gold);
    }
    return {};
}

/** The command words, printed between the synopsis and the flags. */
constexpr std::string_view commands_k = R"(Commands:
  collection create --collection <name> [--modality <name>]
  collection drop --collection <name>
  collection list
  collection import --input <file|-> --collection <name> [--modality <name>] [--ingest sorted|unsorted]
  collection import --input <file> --id-field <field> --memory-limit <size> --collection <name>
  collection export --output <file|-> --collection <name> [--alignment 64|4096] [--batch-rows <count>]
  collection export --output <file> --memory-limit <size> --collection <name>
  snapshot save <path>
  snapshot export <directory>
  snapshot import <directory>

Files ending in .arrows hold Arrow streams, .arrow or .feather Arrow files, and .ndjson or .csv documents. An Arrow
import creates a missing collection, of the --modality given, the schema's ustore.modality, or what its columns spell,
and '-' reads standard input or writes standard output. A snapshot export writes one <name>.arrow per collection into an
empty directory, all at one snapshot, and a snapshot import recreates them on any engine.)";

/** The names @c --format accepts, in the order of @c dataset_formats_k. */
inline constexpr std::array<std::string_view, 5> format_names_k {"automatic", "ndjson", "csv", "arrows", "arrow"};

/** The format each name of @c format_names_k selects. */
inline constexpr std::array<ustore_dataset_format_t, 5> dataset_formats_k {
    ustore_dataset_format_automatic_k, ustore_dataset_format_ndjson_k, ustore_dataset_format_csv_k,
    ustore_dataset_format_arrow_stream_k, ustore_dataset_format_arrow_file_k};

/** The names @c --alignment accepts. */
inline constexpr std::array<std::string_view, 2> alignment_names_k {"64", "4096"};

/** The alignment each name of @c alignment_names_k selects. */
inline constexpr std::array<ustore_size_t, 2> alignments_k {64, 4096};

#pragma region Parsing

/** Every command the tool runs. */
enum class command_t {
    none_k,
    collection_create_k,
    collection_drop_k,
    collection_list_k,
    collection_import_k,
    collection_export_k,
    snapshot_save_k,
    snapshot_export_k,
    snapshot_import_k,
};

/** One invocation: the flags, which view into @c argv and so end in null terminators, and the command they serve. */
struct command_line_t {

    /** The command to run. */
    command_t command = command_t::none_k;

    /** Database directory. */
    std::string_view directory;

    /** The engine to open and the options of its init call. */
    engine_options_t engine;

    /** Collection name. */
    std::string_view collection;

    /** File a document import reads. */
    std::string_view input;

    /** File a document export writes. */
    std::string_view output;

    /** Field holding the keys of imported documents. */
    std::string_view id_field;

    /** Bytes an import or export buffers per batch. */
    bytes_t memory_limit;

    /** Path a snapshot save writes, or the directory a snapshot export writes or a snapshot import reads. */
    std::string_view snapshot_path;

    /** What a created collection holds, by its canonical spelling. */
    std::string_view modality;

    /** How the input or output is spelled, one of @c format_names_k. */
    choice_t format {format_names_k};

    /** Ingest an Arrow import as one commit of @c sorted or @c unsorted keys, or batch it when empty. */
    std::string_view ingest;

    /** Arrow buffer alignment of an export, one of @c alignment_names_k. */
    choice_t alignment {alignment_names_k};

    /** Most rows per Arrow batch of an export, zero for the default. */
    count_t batch_rows;

    /** The format the input or output resolves to, through its extension unless @c --format names one. */
    [[nodiscard]] ustore_dataset_format_t format_of(std::string_view path) const noexcept {
        return ustore_dataset_format_of(bytes_of(path), dataset_formats_k[format.selected]);
    }
};

/** Whether @p format is a text format the document calls read and write. */
[[nodiscard]] constexpr bool is_text(ustore_dataset_format_t format) noexcept {
    return format == ustore_dataset_format_ndjson_k || format == ustore_dataset_format_csv_k;
}

/** Resolves @p positionals into the command of @p line, answering the problem with them, or null when they parse. */
[[nodiscard]] char const* parse_command(std::span<std::string_view const> positionals, command_line_t& line) noexcept {
    std::string_view const object = positionals.size() > 0 ? positionals[0] : "";
    std::string_view const action = positionals.size() > 1 ? positionals[1] : "";
    std::string_view const argument = positionals.size() > 2 ? positionals[2] : "";

    if (object == "collection") {
        if (!argument.empty()) return "Too many arguments";
        if (!line.modality.empty() && !ustore_modality_from_name(bytes_of(line.modality)))
            return "The modality is none of blobs, docs, graph, strings or pages";
        if (!line.ingest.empty() && line.ingest != "sorted" && line.ingest != "unsorted")
            return "The ingest order is neither sorted nor unsorted";
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
            if (line.input.empty() || line.collection.empty()) return "An import needs --input and --collection";
            if (is_text(line.format_of(line.input)) && (line.id_field.empty() || line.memory_limit.value == 0))
                return "An NDJSON or CSV import also needs --id-field and --memory-limit";
            if (is_text(line.format_of(line.input)) && !line.ingest.empty()) return "Only an Arrow import ingests";
            line.command = command_t::collection_import_k;
            return nullptr;
        }
        if (action == "export") {
            if (line.output.empty() || line.collection.empty()) return "An export needs --output and --collection";
            if (is_text(line.format_of(line.output)) && line.memory_limit.value == 0)
                return "An NDJSON or CSV export also needs --memory-limit";
            line.command = command_t::collection_export_k;
            return nullptr;
        }
        return "Invalid collection action";
    }

    // A snapshot lives no longer than the process that took it, so a command saves it or exports it, never keeps it
    if (object == "snapshot") {
        if (action != "save" && action != "export" && action != "import") return "Invalid snapshot action";
        if (argument.empty()) return "The snapshot path is missing";
        line.snapshot_path = argument;
        line.command = action == "save"     ? command_t::snapshot_save_k
                       : action == "export" ? command_t::snapshot_export_k
                                            : command_t::snapshot_import_k;
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
    fmt::println(stderr, style_of(tone_t::failure_k, stderr), "{}: {}", what, outcome.message);
}

/** Resolves @p name to a collection ID through a one-off arena. */
void find_collection(ustore_database_t database, std::string_view name, ustore_collection_id_t& id,
                     ustore_outcome_t* outcome) noexcept {
    auto [arena, arena_status] = arena_t::make();
    if (arena_status.failed()) {
        *outcome = arena_status;
        return;
    }
    std::uint64_t const offsets[2] {0, name.size()};
    ustore_octet_t const* presences = nullptr;
    ustore_collection_id_t const* ids = nullptr;
    ustore_collection_find_t find {};
    find.database = database;
    find.outcome = outcome;
    find.count = 1;
    find.names = u64_tape(name.data(), offsets);
    find.presences = &presences;
    find.collections = &ids;
    find.arena = arena;
    ustore_collection_find(&find);
    if (failed(*outcome)) return;
    if (!(presences[0] & 1)) *outcome = {ustore_status_not_found_k, "No such collection"};
    else id = ids[0];
}

/** Creates a collection under @p name holding what @p line names, documents unless it names a modality. */
[[nodiscard]] bool create_collection(ustore_database_t database, command_line_t const& line) noexcept {
    std::string_view const name = line.collection;
    ustore_outcome_t outcome {};
    ustore_collection_id_t id {};
    ustore_collection_create_t create {};
    create.database = database;
    create.outcome = &outcome;
    create.name = bytes_of(name);
    create.modality = line.modality.empty() ? ustore_modality_docs_k
                                            : ustore_modality_from_name(bytes_of(line.modality));
    create.collection = &id;
    ustore_collection_create(&create);
    if (failed(outcome)) {
        print_failure("Failed to create the collection", outcome);
        return false;
    }
    fmt::println(stderr, style_of(tone_t::success_k, stderr), "Collection '{}' created", name);
    return true;
}

/** Drops the collection @p name addresses, which takes the name with it. */
[[nodiscard]] bool drop_collection(ustore_database_t database, std::string_view name) noexcept {
    ustore_outcome_t outcome {};
    ustore_collection_drop_t drop {};
    drop.database = database;
    drop.outcome = &outcome;
    drop.mode = ustore_drop_collection_k;
    find_collection(database, name, drop.collection, &outcome);
    if (!failed(outcome)) ustore_collection_drop(&drop);
    if (failed(outcome)) {
        print_failure("Failed to drop the collection", outcome);
        return false;
    }
    fmt::println(stderr, style_of(tone_t::success_k, stderr), "Collection '{}' dropped", name);
    return true;
}

/** Prints every collection with its identifier, its name and what it holds. */
[[nodiscard]] bool list_collections(ustore_database_t database) noexcept {
    auto [arena, arena_status] = arena_t::make();
    ustore_outcome_t outcome = arena_status;
    ustore_size_t written = 0;
    ustore_collection_id_t const* ids = nullptr;
    ustore_modality_t const* modalities = nullptr;
    ustore_strings_view_t names {};
    ustore_collection_list_t list {};
    list.database = database;
    list.outcome = &outcome;
    list.collections = &ids;
    list.modalities = &modalities;
    list.names = &names;
    list.written = &written;
    list.arena = arena;
    if (!failed(outcome)) ustore_collection_list(&list);
    for (std::size_t index = 0; !failed(outcome) && index != written; ++index) {
        std::string_view const name = string_of(tape_string(names, index));
        char id_text[uuid_text_length_k];
        uuid_to_text(ids[index].uuid, id_text);
        fmt::println(style_of(tone_t::listing_k, stdout), "{}\t{}\t{}", std::string_view {id_text, uuid_text_length_k},
                     name, ustore_modality_name(modalities[index]));
    }
    if (failed(outcome)) {
        print_failure("Failed to list the collections", outcome);
        return false;
    }
    return true;
}

/** Imports the Arrow input @p line names, creating the collection it names when missing. */
[[nodiscard]] bool import_arrow(ustore_database_t database, command_line_t const& line) noexcept {
    ustore_outcome_t outcome {};
    ustore_size_t written = 0;
    ustore_dataset_import_t importing {};
    importing.database = database;
    importing.outcome = &outcome;
    importing.collection_name = bytes_of(line.collection);
    importing.modality = line.modality.empty() ? ustore_modality_t {}
                                               : ustore_modality_from_name(bytes_of(line.modality));
    importing.path = bytes_of(line.input);
    importing.format = dataset_formats_k[line.format.selected];
    importing.ingestion = line.ingest.empty()       ? ustore_dataset_ingestion_batched_k
                          : line.ingest == "sorted" ? ustore_dataset_ingestion_sorted_k
                                                    : ustore_dataset_ingestion_unsorted_k;
    importing.written = &written;
    ustore_dataset_import(&importing);
    if (failed(outcome)) {
        print_failure("Failed to import", outcome);
        return false;
    }
    fmt::println(stderr, style_of(tone_t::success_k, stderr), "Imported {} rows into '{}'", written, line.collection);
    return true;
}

/** Exports the collection @p line names as Arrow into the file it names, or standard output. */
[[nodiscard]] bool export_arrow(ustore_database_t database, command_line_t const& line) noexcept {
    ustore_outcome_t outcome {};
    ustore_size_t written = 0;
    ustore_collection_id_t collection {};
    find_collection(database, line.collection, collection, &outcome);
    if (!failed(outcome)) {
        ustore_dataset_export_t exporting {};
        exporting.database = database;
        exporting.outcome = &outcome;
        exporting.collection = collection;
        exporting.path = bytes_of(line.output);
        exporting.format = dataset_formats_k[line.format.selected];
        exporting.alignment = alignments_k[line.alignment.selected];
        exporting.batch_rows = line.batch_rows.value;
        exporting.written = &written;
        ustore_dataset_export(&exporting);
    }
    if (failed(outcome)) {
        print_failure("Failed to export", outcome);
        return false;
    }
    fmt::println(stderr, style_of(tone_t::success_k, stderr), "Exported {} rows of '{}'", written, line.collection);
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
        importing.path = bytes_of(line.input);
        importing.max_batch_size = line.memory_limit.value;
        importing.id_field = bytes_of(line.id_field);
        ustore_docs_import(&importing);
    }
    if (failed(outcome)) {
        print_failure("Failed to import", outcome);
        return false;
    }
    fmt::println(stderr, style_of(tone_t::success_k, stderr), "Successfully imported");
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
        exporting.path = bytes_of(line.output);
        exporting.max_batch_size = line.memory_limit.value;
        ustore_docs_export(&exporting);
    }
    if (failed(outcome)) {
        print_failure("Failed to export", outcome);
        return false;
    }
    fmt::println(stderr, style_of(tone_t::success_k, stderr), "Successfully exported");
    return true;
}

/** Takes a snapshot of the head and writes it to @p path. */
[[nodiscard]] bool save_snapshot(ustore_database_t database, std::string_view path) noexcept {
    ustore_outcome_t outcome {};
    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t init {};
    init.database = database;
    init.outcome = &outcome;
    init.snapshot = &snapshot;
    ustore_snapshot_init(&init);
    if (!failed(outcome)) {
        ustore_snapshot_save_t saving {};
        saving.snapshot = snapshot;
        saving.outcome = &outcome;
        saving.path = bytes_of(path);
        ustore_snapshot_save(&saving);
    }
    ustore_snapshot_free(snapshot);
    if (failed(outcome)) {
        print_failure("Failed to save the snapshot", outcome);
        return false;
    }
    fmt::println(stderr, style_of(tone_t::success_k, stderr), "Snapshot saved");
    return true;
}

/** Spells @p directory, a separator, @p stem and the @c .arrow extension into @p file, answering whether it fit. */
[[nodiscard]] bool arrow_file_in(std::string_view directory, std::string_view stem, sz::string_t& file) noexcept {
    return file.assign(sz::string_view_t {directory}) == sz::status_t::success_k &&
           file.append(sz::string_view_t {"/"}) == sz::status_t::success_k &&
           file.append(sz::string_view_t {stem}) == sz::status_t::success_k &&
           file.append(sz::string_view_t {".arrow"}) == sz::status_t::success_k;
}

/** Whether @p name can stand as a file name as it is: no separator, no null, and neither "." nor "..". */
[[nodiscard]] bool is_file_name(std::string_view name) noexcept {
    return !name.empty() && name != "." && name != ".." &&
           name.find_first_of(std::string_view {"/\\\0", 3}) == std::string_view::npos;
}

/**
 *  @brief Takes one snapshot and writes every named collection into the empty directory @p path as one Arrow file,
 *      <tt>&lt;name&gt;.arrow</tt>, whose schema names the collection and its modality, so all read at one stamp.
 *
 *  A name that cannot stand as a file name, or that clashes on a case-insensitive filesystem, takes the collection's
 *  identifier instead; the schema keeps the name either way.
 */
[[nodiscard]] bool export_snapshot(ustore_database_t database, std::string_view path) noexcept {
    std::error_code error;
    std::filesystem::path const directory {path};
    std::filesystem::create_directories(directory, error);
    if (error || !std::filesystem::is_empty(directory, error) || error) {
        fmt::println(stderr, style_of(tone_t::failure_k, stderr), "The snapshot export needs an empty directory: {}",
                     path);
        return false;
    }
    auto [arena, arena_status] = arena_t::make();
    ustore_outcome_t outcome = arena_status;
    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t init {};
    init.database = database;
    init.outcome = &outcome;
    init.snapshot = &snapshot;
    if (!failed(outcome)) ustore_snapshot_init(&init);

    ustore_size_t listed = 0;
    ustore_collection_id_t const* ids = nullptr;
    ustore_strings_view_t names {};
    ustore_collection_list_t list {};
    list.database = database;
    list.outcome = &outcome;
    list.snapshot = snapshot;
    list.collections = &ids;
    list.names = &names;
    list.written = &listed;
    list.arena = arena;
    if (!failed(outcome)) ustore_collection_list(&list);

    std::size_t exported = 0, unnamed = 0;
    for (std::size_t index = 0; !failed(outcome) && index != listed; ++index) {
        std::string_view const name = string_of(tape_string(names, index));
        if (name.empty()) {
            ++unnamed;
            continue;
        }
        char id_text[uuid_text_length_k];
        uuid_to_text(ids[index].uuid, id_text);
        sz::string_t file;
        bool spelled = arrow_file_in(path, name, file);
        if (spelled && (!is_file_name(name) || std::filesystem::exists(std::filesystem::path {file.c_str()}, error)))
            spelled = arrow_file_in(path, std::string_view {id_text, uuid_text_length_k}, file);
        if (!spelled) {
            outcome = {ustore_status_out_of_memory_k, "Failed to spell the path of a collection's file"};
            break;
        }
        ustore_dataset_export_t exporting {};
        exporting.database = database;
        exporting.outcome = &outcome;
        exporting.view = ustore_snapshot_view(snapshot);
        exporting.collection = ids[index];
        exporting.collection_name = bytes_of(name);
        exporting.path = bytes_of(std::string_view {file});
        exporting.format = ustore_dataset_format_arrow_file_k;
        ustore_dataset_export(&exporting);
        exported += !failed(outcome);
    }
    ustore_snapshot_free(snapshot);
    if (failed(outcome)) {
        print_failure("Failed to export the snapshot", outcome);
        return false;
    }
    fmt::println(stderr, style_of(tone_t::success_k, stderr), "Exported {} collections into {}, skipping {} unnamed",
                 exported, path, unnamed);
    return true;
}

/** Imports every <tt>.arrow</tt> file of the directory @p path into the collection its schema names, created when
 *  missing, as one sorted ingest where the engine ingests, since an export writes keys in order, and in batches
 *  otherwise. */
[[nodiscard]] bool import_snapshot(ustore_database_t database, std::string_view path) noexcept {
    std::error_code error;
    st::vector<sz::string_t> files;
    bool held = true;
    for (std::filesystem::directory_iterator entry {std::filesystem::path {path}, error};
         held && !error && entry != std::filesystem::directory_iterator {}; entry.increment(error))
        if (entry->path().extension() == ".arrow")
            held = st::succeeded(files.push_back(sz::string_t {})) &&
                   files.back().assign(sz::string_view_t {entry->path().string()}) == sz::status_t::success_k;
    st::vector<sz_size_t> order;
    held = held && st::succeeded(order.resize(files.size())) &&
           sz::succeeded(sz::argsort(
               files, [](sz::string_t const& file) noexcept { return file.view(); }, std::span<sz_size_t> {order}));
    if (error || !held) {
        fmt::println(stderr, style_of(tone_t::failure_k, stderr), "Failed to list the snapshot directory: {}", path);
        return false;
    }
    ustore_outcome_t outcome {};
    std::size_t imported = 0;
    for (sz_size_t const position : order) {
        sz::string_t const& file = files[position];
        ustore_dataset_import_t importing {};
        importing.database = database;
        importing.outcome = &outcome;
        importing.path = bytes_of(std::string_view {file});
        importing.format = ustore_dataset_format_arrow_file_k;
        importing.ingestion = ustore_dataset_ingestion_sorted_k;
        ustore_dataset_import(&importing);
        // An engine that ingests no stream refuses before landing anything, so the file goes in batches instead
        if (outcome.status == ustore_status_unsupported_k) {
            outcome = {};
            importing.ingestion = ustore_dataset_ingestion_batched_k;
            ustore_dataset_import(&importing);
        }
        if (failed(outcome)) {
            fmt::println(stderr, style_of(tone_t::failure_k, stderr), "Failed to import {}: {}",
                         std::string_view {file}, outcome.message);
            return false;
        }
        ++imported;
    }
    fmt::println(stderr, style_of(tone_t::success_k, stderr), "Imported {} collections from {}", imported, path);
    return true;
}

/** Runs the command of @p line and answers whether it succeeded. */
[[nodiscard]] bool execute(ustore_database_t database, command_line_t const& line) noexcept {
    switch (line.command) {
    case command_t::none_k: return false;
    case command_t::collection_create_k: return create_collection(database, line);
    case command_t::collection_drop_k: return drop_collection(database, line.collection);
    case command_t::collection_list_k: return list_collections(database);
    case command_t::collection_import_k:
        return is_text(line.format_of(line.input)) ? import_documents(database, line) : import_arrow(database, line);
    case command_t::collection_export_k:
        return is_text(line.format_of(line.output)) ? export_documents(database, line) : export_arrow(database, line);
    case command_t::snapshot_save_k: return save_snapshot(database, line.snapshot_path);
    case command_t::snapshot_export_k: return export_snapshot(database, line.snapshot_path);
    case command_t::snapshot_import_k: return import_snapshot(database, line.snapshot_path);
    }
    return false;
}

#pragma endregion Commands

} // namespace unum::ustore::tools

int main(int argc, char* argv[]) {
    using namespace unum::ustore;
    using namespace unum::ustore::tools;

    command_line_t line;
    std::array<flag_t, 14> const flags {{
        {"--directory", "path", "database directory, required", &line.directory},
        {"--engine", "name", "engine to open: ram, rocksdb, leveldb or nvme", &line.engine.name},
        {"--isolation", "name", "isolation of the transactions of ram", &line.engine.isolation},
        {"--engine-transport", "name", "transport of nvme", &line.engine.transport},
        {"--collection", "name", "collection a command acts on", &line.collection},
        {"--input", "file", "file a collection import reads, or '-' for standard input", &line.input},
        {"--output", "file", "file a collection export writes, or '-' for standard output", &line.output},
        {"--id-field", "field", "field holding the keys of imported documents", &line.id_field},
        {"--memory-limit", "size", "bytes an import or export buffers per batch, like 64MB", &line.memory_limit},
        {"--modality", "name", "what a created collection holds: blobs, docs, graph, strings or pages", &line.modality},
        {"--format", "name", "how the input or output is spelled, where '-' is sniffed", &line.format},
        {"--ingest", "order", "ingest as one commit: sorted or unsorted keys", &line.ingest},
        {"--alignment", "bytes", "Arrow buffer alignment of an export", &line.alignment},
        {"--batch-rows", "count", "most rows per Arrow batch of an export", &line.batch_rows},
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
