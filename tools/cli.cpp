/**
 *  @file tools/cli.cpp
 *  @author Ash Vardanian
 *  @date June 5, 2023
 *  @brief Command-line tool over collections, snapshots and datasets, one command per invocation.
 *
 *  Status lines go to standard error, so an export to @c - leaves standard output holding
 *  only Arrow bytes.
 *
 *  The @c manifest and @c recover commands read the files of an NVMe database directly and open
 *  no engine: one prints the manifest a database is opened by, the other writes a fresh one from
 *  the runs alone.
 *
 *  Every flag defaults to its @c USTORE_<FLAG> variable. Each step of a long command prints at most
 *  once a second how far it got. The first SIGINT or SIGTERM stops the command at its next barrier,
 *  which keeps what it landed, removes what it was writing, and exits with 130; a second ends the
 *  process at once.
 */

#include <csignal> // `std::signal`, `std::sig_atomic_t`, `SIGINT`, `SIGTERM`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstdio>  // `stderr`, `std::remove`
#include <cstdlib> // `std::getenv`
#include <cstring> // `std::memcpy`

#include "ustore/types.h" // `USTORE_OS_WINDOWS_`

#if USTORE_OS_WINDOWS_
#include <io.h> // `_isatty`, `_fileno`
#else
#include <unistd.h> // `isatty`
#endif

#include <array>        // `std::array`
#include <chrono>       // `std::chrono::steady_clock`, `std::chrono::seconds`
#include <filesystem>   // `std::filesystem::directory_iterator`, `std::filesystem::is_empty`
#include <span>         // `std::span`
#include <string_view>  // `std::string_view`
#include <system_error> // `std::error_code`

#include <fmt/color.h>  // `fmt::color`, `fmt::fg`, `fmt::text_style`
#include <fmt/format.h> // `fmt::println`, `fmt::format_to_n`

#include <smashtable/basic_vector.hpp> // `st::vector`
#include <stringzilla/find.hpp>        // `sz::split`
#include <stringzilla/sort.hpp>        // `sz::argsort`

#include "ustore/blobs/nvme_key_slot.hpp"      // `key_slot`, `value_layout_t`
#include "ustore/blobs/nvme_sorted_run.hpp"    // `sorted_run_header_t`, `sorted_run_layout`, `sorted_run_magic_k`
#include "ustore/db.h"                         // `ustore_database_t`, `ustore_outcome_t`
#include "ustore/utilities/blocking_files.hpp" // `read_file`, `replace_file_durably`, `open_native_file` & kin
#include "ustore/utilities/nvme_disks.hpp"     // `medium_bytes_of`
#include "ustore/utilities/nvme_queues.hpp"    // `nvme_queues_t`, `io_transport_t`
#include "ustore/utilities/nvme_manifest.hpp"  // `manifest_t`, `default_reserved_record`, `collections_record_t`
#include "ustore/utilities/nvme_slots.hpp"     // `slot_owner_block_t`, `slot_bytes_k`, `slot_payload_offset_k`
#include "ustore/utilities/nvme_volume.hpp"    // `parse_data_address`, `manifest_suffix_k`, `basic_slotted_volume`
#include "ustore/utilities/ranges.hpp"         // `tape_string`
#include "ustore/utilities/types.hpp"          // `u64_tape`, `uuid_to_text`, `uuid_text_length_k`, `bytes_of`

#include "arguments.hpp" // `parse_arguments`, `grammar`, `ustore_variable`, `read_engine_names`
#include "dataset.h"     // `ustore_docs_import`, `ustore_docs_export`

namespace unum::ustore::tools {

/** How a message reads, which decides its color on a terminal. */
enum class tone_t : std::uint8_t {
    failure_k,
    success_k,
    listing_k,
};

#if USTORE_OS_WINDOWS_

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

#pragma region Stopping and Progress

/** Set by the first SIGINT or SIGTERM: every long command stops at its next barrier, keeps what it
 *  landed, removes what it was writing, and the process exits with @ref exit_stopped_k. */
inline volatile std::sig_atomic_t stop_requested = 0;

/** The exit status of a command a signal stopped, as shells report a command SIGINT ended. */
inline constexpr int exit_stopped_k = 130;

/** Asks the command in hand to stop at its next barrier; only a flag is safe to touch here. */
inline void request_stop(int) noexcept {
    stop_requested = 1;
    std::signal(SIGINT, SIG_DFL); // a second signal ends the process at once
    std::signal(SIGTERM, SIG_DFL);
}

using phase_clock_t = std::chrono::steady_clock;

/** The phase a command is in: what the shared progress handler prints, and what a stop names. */
struct verb_phase_t {
    std::string_view name;
    std::string_view unit;
    std::FILE* stream = stderr;
    phase_clock_t::time_point printed {};
};

/** The handler every long command passes, the library's progress callback included: prints the
 *  phase's advance at most once a second, without a total when the call knows none, and answers
 *  cancel once a signal asked the process to stop. */
inline ustore_continuation_t verb_progress(ustore_size_t completed, ustore_size_t total, void* state) noexcept {
    verb_phase_t& phase = *static_cast<verb_phase_t*>(state);
    phase_clock_t::time_point const now = phase_clock_t::now();
    if (now - phase.printed >= std::chrono::seconds(1)) {
        phase.printed = now;
        if (total) fmt::println(phase.stream, "{}: {} of {} {}", phase.name, completed, total, phase.unit);
        else fmt::println(phase.stream, "{}: {} {}", phase.name, completed, phase.unit);
    }
    return stop_requested ? ustore_cancel_k : ustore_continue_k;
}

/** Prints in which phase a signal stopped the command. */
inline void verb_stopped(verb_phase_t const& phase) noexcept {
    fmt::println(phase.stream, "stopped in {}", phase.name);
}

/** Whether @p outcome is the cancel a signal asked for, which ends a command without failing it. */
[[nodiscard]] bool stopped(ustore_outcome_t const& outcome) noexcept {
    return outcome.status == ustore_status_cancelled_k && stop_requested;
}

#pragma endregion Stopping and Progress

/** The command words, printed between the synopsis and the flags. */
constexpr std::string_view commands_k = R"(Commands:
  collection create --collection <name> [--modality <name>]
  collection drop --collection <name>
  collection list
  collection import --input <file|-> --collection <name> [--modality <name>] [--ingest <order>]
  collection import --input <file> --id-field <field> --memory-limit <size> --collection <name>
  collection export --output <file|-> --collection <name> [--alignment 64|4096] [--batch-rows <count>]
  collection export --output <file> --memory-limit <size> --collection <name>
  snapshot save <path>
  snapshot export <directory>
  snapshot import <directory>
  manifest <manifest path or address> [--database <name>]
  recover <data directories or addresses...> --database <name>

Files ending in .arrows hold Arrow streams, .arrow or .feather Arrow files, and .ndjson or .csv documents. An Arrow
import creates a missing collection, of the --modality given, the schema's ustore.modality, or what its columns spell,
and '-' reads standard input or writes standard output. A snapshot export writes one <name>.arrow per collection into an
empty directory, all at one snapshot, and a snapshot import recreates them on any engine. The manifest and recover
commands read an NVMe database's files without opening it: manifest prints <database>.ustore.json, or the copy a raw
address's manifest slot points at, and recover writes a fresh manifest naming every run found, each on level 0.

Each step of a long command prints "<phase>: <completed> of <total> <unit>" at most once a second. The first SIGINT or
SIGTERM stops it at its next barrier with "stopped in <phase>" and exit status 130; a second ends it at once.)";

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

/** The names @c --ingest accepts, in the order of @c ustore_dataset_ingestion_t. */
inline constexpr std::array<std::string_view, 3> ingestion_names_k {"batched", "sorted", "unsorted"};

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
    manifest_print_k,
    database_recover_k,
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

    /** How an Arrow import lands, one of @c ingestion_names_k: in batches, or as one commit of
     *  sorted or unsorted keys. */
    choice_t ingest {ingestion_names_k};

    /** Arrow buffer alignment of an export, one of @c alignment_names_k. */
    choice_t alignment {alignment_names_k};

    /** Most rows per Arrow batch of an export, zero for the default. */
    count_t batch_rows;

    /** The NVMe database a manifest or a recovery names. */
    std::string_view database;

    /** The manifest path or the data directory addresses a manifest or a recovery reads. */
    std::span<std::string_view const> addresses;

    /** Whether the command reads files directly rather than through an open engine. */
    [[nodiscard]] bool opens_no_engine() const noexcept {
        return command == command_t::manifest_print_k || command == command_t::database_recover_k;
    }

    /** Whether the command changes the database, which then flushes before it closes. */
    [[nodiscard]] bool writes() const noexcept {
        return command == command_t::collection_create_k || command == command_t::collection_drop_k ||
               command == command_t::collection_import_k || command == command_t::snapshot_import_k;
    }

    /** The format the input or output resolves to, through its extension unless @c --format names one. */
    [[nodiscard]] ustore_dataset_format_t format_of(std::string_view path) const noexcept {
        return ustore_dataset_format_of(util::bytes_of(path), dataset_formats_k[format.selected]);
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
            if (is_text(line.format_of(line.input)) && line.ingest.selected != ustore_dataset_ingestion_batched_k)
                return "Only an Arrow import ingests";
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
    if (object == "manifest") {
        if (action.empty() || !argument.empty()) return "A manifest command takes one path or address";
        line.addresses = positionals.subspan(1);
        line.command = command_t::manifest_print_k;
        return nullptr;
    }
    if (object == "recover") {
        if (action.empty()) return "A recovery needs at least one data directory or address";
        if (line.database.empty()) return "A recovery needs --database";
        line.addresses = positionals.subspan(1);
        line.command = command_t::database_recover_k;
        return nullptr;
    }
    return object.empty() ? "The command is missing" : "Unknown command";
}

/** Prints the one-line error of the first named flag of @p line whose text the C interface spells
 *  nowhere, answering whether every one parses. */
[[nodiscard]] bool names_parse(command_line_t const& line) noexcept {
    engine_names_t names;
    unnamed_flag_t unnamed = read_engine_names(line.engine, names);
    ustore_modality_t modality {};
    if (unnamed.flag.empty() && !line.modality.empty() &&
        assign_named_(line.modality, modality, &ustore_modality_from_name) != argument_status_t::parsed_k)
        unnamed = {"--modality", line.modality, "blobs, docs, graph, strings or pages"};
    if (unnamed.flag.empty()) return true;
    fmt::println(stderr, "{}=\"{}\" does not parse, expected {}", unnamed.flag, unnamed.value, unnamed.grammar);
    return false;
}

#pragma endregion Parsing

#pragma region Commands

/** Whether @p outcome holds a failure. */
[[nodiscard]] bool failed(ustore_outcome_t const& outcome) noexcept {
    return outcome.status != ustore_status_success_k;
}

/** The arena the tasks of one command fill, made where @p executor computes,
 *  freed on destruction. */
class task_arena_t {
    ustore_arena_t arena_ = nullptr;

  public:

    /** Makes the arena unless @p outcome already holds a failure, reporting its own into it. */
    task_arena_t(ustore_executor_t executor, ustore_outcome_t* outcome) noexcept {
        if (failed(*outcome)) return;
        ustore_arena_init_t init {.arena = &arena_, .executor = executor, .outcome = outcome};
        ustore_arena_init(&init);
    }
    task_arena_t(task_arena_t const&) = delete;
    task_arena_t& operator=(task_arena_t const&) = delete;
    ~task_arena_t() noexcept { ustore_arena_free(arena_); }

    [[nodiscard]] ustore_arena_t get() const noexcept { return arena_; }
};

/** Prints a red failure line with the database outcome. */
void print_failure(char const* what, ustore_outcome_t const& outcome) noexcept {
    fmt::println(stderr, style_of(tone_t::failure_k, stderr), "{}: {}", what, outcome.message);
}

/** Resolves @p name to a collection ID through a one-off arena. */
void find_collection(ustore_database_t database, ustore_executor_t executor, std::string_view name,
                     ustore_collection_id_t& id, ustore_outcome_t* outcome) noexcept {
    task_arena_t const arena {executor, outcome};
    if (failed(*outcome)) return;
    std::uint64_t const offsets[2] {0, name.size()};
    ustore_octet_t const* presences = nullptr;
    ustore_collection_id_t const* ids = nullptr;
    ustore_collection_find_t find {};
    find.database = database;
    find.executor = executor;
    find.outcome = outcome;
    find.count = 1;
    find.names = util::u64_tape(name.data(), offsets);
    find.presences = &presences;
    find.collections = &ids;
    find.arena = arena.get();
    ustore_collection_find(&find);
    if (failed(*outcome)) return;
    if (!(presences[0] & 1)) *outcome = {ustore_status_not_found_k, "No such collection"};
    else id = ids[0];
}

/** Creates a collection under @p name holding what @p line names, documents unless it names a modality. */
[[nodiscard]] bool create_collection(ustore_database_t database, ustore_executor_t executor,
                                     command_line_t const& line) noexcept {
    std::string_view const name = line.collection;
    ustore_outcome_t outcome {};
    ustore_collection_id_t id {};
    ustore_collection_create_t create {};
    create.database = database;
    create.executor = executor;
    create.outcome = &outcome;
    create.name = util::bytes_of(name);
    create.modality = ustore_modality_docs_k;
    if (!line.modality.empty()) ustore_modality_from_name(util::bytes_of(line.modality), &create.modality);
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
[[nodiscard]] bool drop_collection(ustore_database_t database, ustore_executor_t executor,
                                   std::string_view name) noexcept {
    ustore_outcome_t outcome {};
    ustore_collection_drop_t drop {};
    drop.database = database;
    drop.executor = executor;
    drop.outcome = &outcome;
    drop.mode = ustore_drop_collection_k;
    find_collection(database, executor, name, drop.collection, &outcome);
    if (!failed(outcome)) ustore_collection_drop(&drop);
    if (failed(outcome)) {
        print_failure("Failed to drop the collection", outcome);
        return false;
    }
    fmt::println(stderr, style_of(tone_t::success_k, stderr), "Collection '{}' dropped", name);
    return true;
}

/** Prints every collection with its identifier, its name and what it holds. */
[[nodiscard]] bool list_collections(ustore_database_t database, ustore_executor_t executor) noexcept {
    ustore_outcome_t outcome {};
    task_arena_t const arena {executor, &outcome};
    ustore_size_t written = 0;
    ustore_collection_id_t const* ids = nullptr;
    ustore_modality_t const* modalities = nullptr;
    ustore_strings_view_t names {};
    ustore_collection_list_t list {};
    list.database = database;
    list.executor = executor;
    list.outcome = &outcome;
    list.collections = &ids;
    list.modalities = &modalities;
    list.names = &names;
    list.written = &written;
    list.arena = arena.get();
    if (!failed(outcome)) ustore_collection_list(&list);
    for (std::size_t index = 0; !failed(outcome) && index != written; ++index) {
        std::string_view const name = util::chars_of(util::tape_string(names, index));
        char id_text[util::uuid_text_length_k];
        util::uuid_to_text(ids[index].uuid, id_text);
        fmt::println(style_of(tone_t::listing_k, stdout), "{}\t{}\t{}",
                     std::string_view {id_text, util::uuid_text_length_k}, name,
                     ustore_modality_name(modalities[index]));
    }
    if (failed(outcome)) {
        print_failure("Failed to list the collections", outcome);
        return false;
    }
    return true;
}

/** Imports the Arrow input @p line names, creating the collection it names when missing. */
[[nodiscard]] bool import_arrow(ustore_database_t database, ustore_executor_t executor,
                                command_line_t const& line) noexcept {
    ustore_outcome_t outcome {};
    task_arena_t const arena {executor, &outcome};
    // Several inputs come as one comma-separated list, laid out as a tape of paths
    sz::string_t paths;
    st::vector<std::uint64_t> offsets;
    if (!failed(outcome) && st::failed(offsets.push_back(std::uint64_t {0})))
        outcome = {ustore_status_out_of_memory_k, "Failed to list the inputs"};
    for (sz::string_view_t const path : sz::split(sz::string_view_t {line.input}, sz::string_view_t {","}))
        if (!failed(outcome) && (paths.append(path.data(), path.size()) != sz::status_t::success_k ||
                                 st::failed(offsets.push_back(std::uint64_t {paths.size()}))))
            outcome = {ustore_status_out_of_memory_k, "Failed to list the inputs"};
    ustore_size_t written = 0;
    ustore_dataset_import_t importing {};
    importing.database = database;
    importing.executor = executor;
    importing.outcome = &outcome;
    importing.arena = arena.get();
    importing.collection_name = util::bytes_of(line.collection);
    importing.modality = ustore_modality_t {};
    if (!line.modality.empty()) ustore_modality_from_name(util::bytes_of(line.modality), &importing.modality);
    importing.paths = util::u64_tape(paths.data(), offsets.data());
    importing.paths_count = offsets.size() - 1;
    importing.format = dataset_formats_k[line.format.selected];
    importing.ingestion = static_cast<ustore_dataset_ingestion_t>(line.ingest.selected);
    importing.written = &written;
    verb_phase_t phase {.name = "import", .unit = "bytes"};
    importing.progress = verb_progress;
    importing.progress_state = &phase;
    if (!failed(outcome)) ustore_dataset_import(&importing);
    if (stopped(outcome)) {
        verb_stopped(phase);
        return true;
    }
    if (failed(outcome)) {
        print_failure("Failed to import", outcome);
        return false;
    }
    fmt::println(stderr, style_of(tone_t::success_k, stderr), "Imported {} rows into '{}'", written, line.collection);
    return true;
}

/** Exports the collection @p line names as Arrow into the file it names, or standard output. */
[[nodiscard]] bool export_arrow(ustore_database_t database, ustore_executor_t executor,
                                command_line_t const& line) noexcept {
    ustore_outcome_t outcome {};
    ustore_size_t written = 0;
    verb_phase_t phase {.name = "export", .unit = "rows"};
    ustore_collection_id_t collection {};
    find_collection(database, executor, line.collection, collection, &outcome);
    task_arena_t const arena {executor, &outcome};
    if (!failed(outcome)) {
        ustore_dataset_export_t exporting {};
        exporting.database = database;
        exporting.executor = executor;
        exporting.outcome = &outcome;
        exporting.arena = arena.get();
        exporting.collection = collection;
        exporting.path = util::bytes_of(line.output);
        exporting.format = dataset_formats_k[line.format.selected];
        exporting.alignment = alignments_k[line.alignment.selected];
        exporting.batch_rows = line.batch_rows.value;
        exporting.written = &written;
        exporting.progress = verb_progress;
        exporting.progress_state = &phase;
        ustore_dataset_export(&exporting);
    }
    if (stopped(outcome)) {
        verb_stopped(phase);
        return true;
    }
    if (failed(outcome)) {
        print_failure("Failed to export", outcome);
        return false;
    }
    fmt::println(stderr, style_of(tone_t::success_k, stderr), "Exported {} rows of '{}'", written, line.collection);
    return true;
}

/** Imports the documents of the file @p line names into the collection it names. */
[[nodiscard]] bool import_documents(ustore_database_t database, ustore_executor_t executor,
                                    command_line_t const& line) noexcept {
    ustore_outcome_t outcome {};
    verb_phase_t phase {.name = "import", .unit = "bytes"};
    ustore_collection_id_t collection {};
    find_collection(database, executor, line.collection, collection, &outcome);
    task_arena_t const arena {executor, &outcome};
    if (!failed(outcome)) {
        ustore_docs_import_t importing {};
        importing.database = database;
        importing.executor = executor;
        importing.outcome = &outcome;
        importing.arena = arena.get();
        importing.collection = collection;
        importing.path = util::bytes_of(line.input);
        importing.max_batch_size = line.memory_limit.value;
        importing.id_field = util::bytes_of(line.id_field);
        importing.progress = verb_progress;
        importing.progress_state = &phase;
        ustore_docs_import(&importing);
    }
    if (stopped(outcome)) {
        verb_stopped(phase);
        return true;
    }
    if (failed(outcome)) {
        print_failure("Failed to import", outcome);
        return false;
    }
    fmt::println(stderr, style_of(tone_t::success_k, stderr), "Successfully imported");
    return true;
}

/** Exports the collection @p line names into the file it names. */
[[nodiscard]] bool export_documents(ustore_database_t database, ustore_executor_t executor,
                                    command_line_t const& line) noexcept {
    ustore_outcome_t outcome {};
    verb_phase_t phase {.name = "export", .unit = "documents"};
    ustore_collection_id_t collection {};
    find_collection(database, executor, line.collection, collection, &outcome);
    task_arena_t const arena {executor, &outcome};
    if (!failed(outcome)) {
        ustore_docs_export_t exporting {};
        exporting.database = database;
        exporting.executor = executor;
        exporting.outcome = &outcome;
        exporting.arena = arena.get();
        exporting.collection = collection;
        exporting.path = util::bytes_of(line.output);
        exporting.max_batch_size = line.memory_limit.value;
        exporting.progress = verb_progress;
        exporting.progress_state = &phase;
        ustore_docs_export(&exporting);
    }
    if (stopped(outcome)) {
        verb_stopped(phase);
        return true;
    }
    if (failed(outcome)) {
        print_failure("Failed to export", outcome);
        return false;
    }
    fmt::println(stderr, style_of(tone_t::success_k, stderr), "Successfully exported");
    return true;
}

/** Takes a snapshot of the head and writes it to @p path. */
[[nodiscard]] bool save_snapshot(ustore_database_t database, ustore_executor_t executor,
                                 std::string_view path) noexcept {
    ustore_outcome_t outcome {};
    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t init {};
    init.database = database;
    init.executor = executor;
    init.outcome = &outcome;
    init.snapshot = &snapshot;
    ustore_snapshot_init(&init);
    verb_phase_t phase {.name = "save", .unit = "bytes"};
    if (!failed(outcome)) {
        ustore_snapshot_save_t saving {};
        saving.snapshot = snapshot;
        saving.executor = executor;
        saving.outcome = &outcome;
        saving.path = util::bytes_of(path);
        saving.progress = verb_progress;
        saving.progress_state = &phase;
        ustore_snapshot_save(&saving);
    }
    ustore_snapshot_free(snapshot);
    if (stopped(outcome)) {
        verb_stopped(phase);
        return true;
    }
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
 *
 *  Each file lands whole from a temporary beside it, and a stop or a failure removes every file
 *  the export landed, so no partial export stays behind.
 */
[[nodiscard]] bool export_snapshot(ustore_database_t database, ustore_executor_t executor,
                                   std::string_view path) noexcept {
    std::error_code error;
    std::filesystem::path const directory {path};
    std::filesystem::create_directories(directory, error);
    if (error || !std::filesystem::is_empty(directory, error) || error) {
        fmt::println(stderr, style_of(tone_t::failure_k, stderr), "The snapshot export needs an empty directory: {}",
                     path);
        return false;
    }
    // The export rewinds its own arena, while the listing's names stay readable in the other
    ustore_outcome_t outcome {};
    task_arena_t const arena {executor, &outcome};
    task_arena_t const exported_arena {executor, &outcome};
    ustore_snapshot_t snapshot = nullptr;
    ustore_snapshot_init_t init {};
    init.database = database;
    init.executor = executor;
    init.outcome = &outcome;
    init.snapshot = &snapshot;
    if (!failed(outcome)) ustore_snapshot_init(&init);

    ustore_size_t listed = 0;
    ustore_collection_id_t const* ids = nullptr;
    ustore_strings_view_t names {};
    ustore_collection_list_t list {};
    list.database = database;
    list.executor = executor;
    list.outcome = &outcome;
    list.snapshot = snapshot;
    list.collections = &ids;
    list.names = &names;
    list.written = &listed;
    list.arena = arena.get();
    if (!failed(outcome)) ustore_collection_list(&list);

    verb_phase_t collections {.name = "export", .unit = "collections"};
    verb_phase_t rows {.name = "export", .unit = "rows"};
    st::vector<sz::string_t> written;
    std::size_t exported = 0, unnamed = 0;
    for (std::size_t index = 0; !failed(outcome) && index != listed; ++index) {
        if (verb_progress(index, listed, &collections) == ustore_cancel_k) {
            outcome = {ustore_status_cancelled_k, "Stopped between collections"};
            break;
        }
        std::string_view const name = util::chars_of(util::tape_string(names, index));
        if (name.empty()) {
            ++unnamed;
            continue;
        }
        char id_text[util::uuid_text_length_k];
        util::uuid_to_text(ids[index].uuid, id_text);
        sz::string_t file;
        bool spelled = arrow_file_in(path, name, file);
        if (spelled && (!is_file_name(name) || std::filesystem::exists(std::filesystem::path {file.c_str()}, error)))
            spelled = arrow_file_in(path, std::string_view {id_text, util::uuid_text_length_k}, file);
        if (!spelled) {
            outcome = {ustore_status_out_of_memory_k, "Failed to spell the path of a collection's file"};
            break;
        }
        ustore_dataset_export_t exporting {};
        exporting.database = database;
        exporting.executor = executor;
        exporting.outcome = &outcome;
        exporting.arena = exported_arena.get();
        exporting.view = ustore_snapshot_view(snapshot);
        exporting.collection = ids[index];
        exporting.collection_name = util::bytes_of(name);
        exporting.path = util::bytes_of(std::string_view {file});
        exporting.format = ustore_dataset_format_arrow_file_k;
        exporting.progress = verb_progress;
        exporting.progress_state = &rows;
        ustore_dataset_export(&exporting);
        if (failed(outcome)) break;
        ++exported;
        // Only a landed file joins the ones a later stop or failure removes, as a refused export
        // removed its own temporary and left the target alone
        if (!st::succeeded(written.push_back(sz::string_t {})) ||
            written.back().assign(sz::string_view_t {file.view()}) != sz::status_t::success_k) {
            [[maybe_unused]] int const removed = std::remove(file.c_str());
            outcome = {ustore_status_out_of_memory_k, "Failed to record the path of a collection's file"};
        }
    }
    ustore_snapshot_free(snapshot);
    if (failed(outcome))
        for (sz::string_t const& file : written) [[maybe_unused]]
            int const removed = std::remove(file.c_str());
    if (stopped(outcome)) {
        verb_stopped(collections);
        return true;
    }
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
 *  otherwise. A stop keeps the collections imported before it. */
[[nodiscard]] bool import_snapshot(ustore_database_t database, ustore_executor_t executor,
                                   std::string_view path) noexcept {
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
    task_arena_t const arena {executor, &outcome};
    if (failed(outcome)) {
        print_failure("Failed to import the snapshot", outcome);
        return false;
    }
    std::size_t imported = 0;
    verb_phase_t collections {.name = "import", .unit = "files"};
    verb_phase_t bytes {.name = "import", .unit = "bytes"};
    for (sz_size_t const position : order) {
        if (verb_progress(imported, files.size(), &collections) == ustore_cancel_k) {
            verb_stopped(collections);
            return true;
        }
        sz::string_t const& file = files[position];
        ustore_dataset_import_t importing {};
        importing.database = database;
        importing.executor = executor;
        importing.outcome = &outcome;
        importing.arena = arena.get();
        std::uint64_t const offsets[2] {0, file.size()};
        importing.paths = util::u64_tape(file.data(), offsets);
        importing.paths_count = 1;
        importing.format = ustore_dataset_format_arrow_file_k;
        importing.ingestion = ustore_dataset_ingestion_sorted_k;
        importing.progress = verb_progress;
        importing.progress_state = &bytes;
        ustore_dataset_import(&importing);
        // An engine that ingests no stream refuses before landing anything, so the file goes in batches instead
        if (outcome.status == ustore_status_unsupported_k) {
            outcome = {};
            importing.ingestion = ustore_dataset_ingestion_batched_k;
            ustore_dataset_import(&importing);
        }
        if (stopped(outcome)) {
            verb_stopped(bytes);
            return true;
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

#pragma region Manifests

/** Prints a red failure line with a status of the NVMe layer. */
void print_failure(char const* what, util::status_t const& status) noexcept {
    char const* const detail = status.note ? status.note
                                           : ustore_status_name(static_cast<ustore_status_t>(status.code));
    fmt::println(stderr, style_of(tone_t::failure_k, stderr), "{}: {}", what, detail);
}

/** Copies @p text into @p into with a terminator, refusing a path the platform cannot hold. */
util::status_t terminated(std::string_view text, std::span<char> into) noexcept {
    if (text.size() >= into.size())
        return {util::status_code_t::invalid_argument_k, "A path outgrew the platform's limit"};
    std::memcpy(into.data(), text.data(), text.size());
    into[text.size()] = '\0';
    return {};
}

/** Reads exactly @p into's bytes of @p file from @p offset, refusing a medium that
 *  ends before them. */
util::status_t read_exactly(util::native_file_t file, std::uint64_t offset, std::span<util::byte_t> into) noexcept {
    auto [event, event_status] = util::native_file_event_t::make();
    if (event_status.failed()) return event_status;
    auto [moved, read_status] = util::read_native_file(file, offset, into, event);
    if (read_status.failed()) return read_status;
    if (moved != into.size()) return {util::status_code_t::io_k, "The medium ended before the bytes asked for"};
    return {};
}

/** Writes all of @p from into @p file at @p offset, refusing a medium that took fewer bytes. */
util::status_t write_exactly(util::native_file_t file, std::uint64_t offset,
                             std::span<util::byte_t const> from) noexcept {
    auto [event, event_status] = util::native_file_event_t::make();
    if (event_status.failed()) return event_status;
    auto [moved, write_status] = util::write_native_file(file, offset, from, event);
    if (write_status.failed()) return write_status;
    if (moved != from.size())
        return {util::status_code_t::capacity_exhausted_k, "The medium took fewer bytes than given"};
    return {};
}

/** The owner block opening the slot at @p offset of @p file. */
util::expected<util::slot_owner_block_t> read_owner_block(util::native_file_t file, std::uint64_t offset) noexcept {
    util::slot_owner_block_t owner {};
    if (util::status_t const read = read_exactly(file, offset, std::as_writable_bytes(std::span {&owner, 1}));
        read.failed())
        return read;
    return owner;
}

/** Bytes of each of a manifest slot's two JSON regions, as a slotted volume lays them out. */
inline constexpr std::uint64_t manifest_region_bytes_k =
    util::basic_slotted_volume<util::memory_disk<fu::standard_atomic_ref, fu::standard_yield_t>, fu::standard_yield_t,
                               st::default_allocator<util::byte_t>>::manifest_region_bytes_k;

/** Prints @p bytes to standard output, answering whether every byte went out. */
[[nodiscard]] bool print_bytes(std::span<char const> bytes) noexcept {
    return std::fwrite(bytes.data(), 1, bytes.size(), stdout) == bytes.size() && std::fflush(stdout) == 0;
}

/** Reads the manifest copy the manifest slot of the raw medium at @p address points at
 *  into @p json. */
util::status_t read_raw_manifest(util::data_address_t const& address, st::vector<char>& json) noexcept {
    char path[util::path_max_bytes_k];
    if (util::status_t const copied = terminated(address.location, path); copied.failed()) return copied;
    auto [file, open_status] = util::open_native_file(path, util::file_access_t::read_only_k,
                                                      util::file_buffering_t::buffered_k);
    if (open_status.failed()) return open_status;
    util::status_t result;
    auto [owner, owner_status] = read_owner_block(file, address.byte_base);
    if (owner_status.failed()) result = owner_status;
    else if (owner.magic != util::slot_owner_magic_k || owner.format_version != util::slot_format_version_k)
        result = {util::status_code_t::not_found_k, "The address holds no database of this layout"};
    else if (owner.manifest_bytes == 0)
        result = {util::status_code_t::not_found_k, "The database has published no manifest"};
    else if (owner.manifest_bytes > manifest_region_bytes_k ||
             (owner.manifest_offset != address.byte_base + util::slot_payload_offset_k &&
              owner.manifest_offset != address.byte_base + util::slot_payload_offset_k + manifest_region_bytes_k))
        result = {util::status_code_t::malformed_k, "The manifest slot points outside its regions"};
    else if (json.resize(static_cast<std::size_t>(owner.manifest_bytes)) != st::status_t::success_k)
        result = {util::status_code_t::out_of_memory_k, "No room to read the manifest"};
    else result = read_exactly(file, owner.manifest_offset, std::as_writable_bytes(std::span<char> {json}));
    [[maybe_unused]] util::status_t const closed = util::close_native_file(file);
    return result;
}

/** The path of @p database's manifest inside the directory @p location, into @p into. */
util::status_t manifest_path(std::string_view location, std::string_view database, std::span<char> into) noexcept {
    char directory[util::path_max_bytes_k], name[util::path_max_bytes_k];
    if (util::status_t const copied = terminated(location, directory); copied.failed()) return copied;
    if (database.size() + util::manifest_suffix_k.size() >= sizeof(name))
        return {util::status_code_t::invalid_argument_k, "A database name outgrew the platform's limit"};
    std::memcpy(name, database.data(), database.size());
    std::memcpy(name + database.size(), util::manifest_suffix_k.data(), util::manifest_suffix_k.size());
    name[database.size() + util::manifest_suffix_k.size()] = '\0';
    return util::join_path(directory, name, into);
}

/** Prints the manifest @p line names: a @c .ustore.json path, a directory with @c --database, or
 *  a raw address. */
[[nodiscard]] bool print_manifest(command_line_t const& line) noexcept {
    std::string_view const address = line.addresses.front();
    st::vector<char> json;
    char path[util::path_max_bytes_k];
    util::status_t status;
    if (address.ends_with(util::manifest_suffix_k)) {
        status = terminated(address, path);
        if (status.succeeded()) status = util::read_file(path, json);
    }
    else {
        auto [parsed, parse_status] = util::parse_data_address(address);
        status = parse_status;
        if (status.failed()) {}
        else if (parsed.kind == util::data_address_kind_t::spdk_k)
            status = {util::status_code_t::unsupported_k, "SPDK namespaces are read through a mounted database"};
        else if (parsed.kind == util::data_address_kind_t::raw_k) status = read_raw_manifest(parsed, json);
        else if (line.database.empty())
            status = {util::status_code_t::invalid_argument_k, "A directory needs --database"};
        else {
            status = manifest_path(parsed.location, line.database, path);
            if (status.succeeded()) status = util::read_file(path, json);
        }
    }
    if (status.failed()) {
        print_failure("Failed to read the manifest", status);
        return false;
    }
    return print_bytes(std::span<char const> {json});
}

#pragma endregion Manifests

#pragma region Recovery

/** Bytes the run @p header opens spans when its cells are @p key_slot_type_, refusing a header of
 *  another key slot. */
template <typename key_slot_type_>
util::expected<std::uint64_t> run_bytes_as(blobs::sorted_run_header_t const& header) noexcept {
    using layout_t = blobs::sorted_run_layout<key_slot_type_>;
    if (header.key_slot_bytes != sizeof(key_slot_type_) || header.branching != layout_t::branching_k)
        return util::status_t {util::status_code_t::malformed_k, "A run's cells match no key slot of this build"};
    return layout_t::blob_bytes(header);
}

/** Bytes the run @p header opens spans, by the key slot its value layout names. */
util::expected<std::uint64_t> run_bytes_of(blobs::sorted_run_header_t const& header) noexcept {
    using blobs::key_slot;
    using blobs::value_layout_t;
    switch (header.value_layout) {
    case value_layout_t::inline_12_bytes_k:
        return run_bytes_as<key_slot<st::key128_t, value_layout_t::inline_12_bytes_k>>(header);
    case value_layout_t::inline_44_bytes_k:
        return run_bytes_as<key_slot<st::key128_t, value_layout_t::inline_44_bytes_k>>(header);
    case value_layout_t::fixed_width_k:
        return run_bytes_as<key_slot<st::key128_t, value_layout_t::fixed_width_k>>(header);
    case value_layout_t::variable_width_k:
        return run_bytes_as<key_slot<st::key128_t, value_layout_t::variable_width_k>>(header);
    case value_layout_t::uuid_k: {
        using layout_t =
            blobs::strings_run_layout<key_slot<st::key128_t, value_layout_t::uuid_k, blobs::key_tails_t::present_k>>;
        if (header.key_slot_bytes != sizeof(typename layout_t::key_slot_t) ||
            header.branching != layout_t::keys_per_row_k)
            return util::status_t {util::status_code_t::malformed_k, "A run's cells match no key slot of this build"};
        return layout_t::blob_bytes(header);
    }
    default: return util::status_t {util::status_code_t::malformed_k, "A run names no value layout of this build"};
    }
}

/** One run a recovery found: where it lies, as the manifest will name it, and the header
 *  it opens with. */
struct found_run_t {
    util::manifest_file_t file;
    blobs::sorted_run_header_t header;
};

/** What a recovery gathers before it writes the manifest: the runs found and the manifest
 *  they go into. */
struct recovery_t {
    util::manifest_t manifest;
    st::vector<found_run_t> runs;
};

/** Adds the run @p header opens, at @p file's place, to @p recovery, on level 0. */
util::status_t recover_run(recovery_t& recovery, blobs::sorted_run_header_t const& header,
                           util::manifest_file_t file) noexcept {
    file.level = 0;
    if (header.newest_stamp > recovery.manifest.durable_stamp) recovery.manifest.durable_stamp = header.newest_stamp;
    if (recovery.runs.push_back(found_run_t {std::move(file), header}) != st::status_t::success_k)
        return {util::status_code_t::out_of_memory_k, "No room to record a recovered run"};
    return {};
}

/** Reads the header at @p offset of @p file, answering whether a run of this layout opens there. */
util::expected<bool> read_run_header(util::native_file_t file, std::uint64_t offset,
                                     blobs::sorted_run_header_t& header) noexcept {
    if (util::status_t const read = read_exactly(file, offset, std::as_writable_bytes(std::span {&header, 1}));
        read.failed())
        return read;
    return header.magic == blobs::sorted_run_magic_k && header.format_version == blobs::sorted_run_format_version_k;
}

/** The status a recovery answers once a signal asked it to stop, which writes no manifest. */
inline util::status_t stopped_recovery() noexcept {
    return {util::status_code_t::cancelled_k, "Stopped before the manifest"};
}

/** Adds every run file of the database in the directory @p location, data directory @p medium,
 *  to @p recovery, stopping between files once a signal asks. */
util::status_t recover_directory(recovery_t& recovery, std::string_view location, std::uint16_t medium) noexcept {
    char directory[util::path_max_bytes_k];
    if (util::status_t const copied = terminated(location, directory); copied.failed()) return copied;
    util::status_t first;
    util::status_t const listed = util::list_directory(directory, [&](std::string_view name) noexcept {
        util::uuid128_t const run = recovery.manifest.run_of(name);
        if (stop_requested && first.succeeded()) first = stopped_recovery();
        if (!run.is_valid() || first.failed()) return;
        char file_name[util::path_max_bytes_k], path[util::path_max_bytes_k];
        if (util::status_t const copied = terminated(name, file_name); copied.failed()) {
            first = copied;
            return;
        }
        if (util::status_t const joined = util::join_path(directory, file_name, path); joined.failed()) {
            first = joined;
            return;
        }
        auto [file, open_status] = util::open_native_file(path, util::file_access_t::read_only_k,
                                                          util::file_buffering_t::buffered_k);
        if (open_status.failed()) {
            first = open_status;
            return;
        }
        blobs::sorted_run_header_t header {};
        auto [opens, read_status] = read_run_header(file, 0, header);
        std::uint64_t const bytes = util::native_file_capacity(file);
        [[maybe_unused]] util::status_t const closed = util::close_native_file(file);
        if (read_status.failed()) first = read_status;
        else if (!opens || header.content_id != run)
            fmt::println(stderr, style_of(tone_t::failure_k, stderr), "Skipping {}, which holds no run of its name",
                         name);
        else {
            util::manifest_file_t found;
            found.run = run;
            found.bytes = bytes;
            found.medium = medium;
            first = recover_run(recovery, header, found);
        }
    });
    return listed.failed() ? listed : first;
}

/** Adds every run in the slots the database holds on the raw medium at @p address to @p recovery,
 *  reading the database's ID from data directory 0's manifest slot when @p medium is zero, and
 *  stopping between slots once a signal asks. */
util::status_t recover_raw(recovery_t& recovery, util::data_address_t const& address, std::uint16_t medium) noexcept {
    char path[util::path_max_bytes_k];
    if (util::status_t const copied = terminated(address.location, path); copied.failed()) return copied;
    auto [file, open_status] = util::open_native_file(path, util::file_access_t::read_only_k,
                                                      util::file_buffering_t::buffered_k);
    if (open_status.failed()) return open_status;
    std::uint64_t const capacity = util::medium_bytes_of(file);
    std::uint64_t usable = capacity > address.byte_base ? capacity - address.byte_base : 0;
    if (address.byte_limit != 0 && address.byte_limit < usable) usable = address.byte_limit;
    util::status_t result;
    for (std::uint64_t slot = 0; result.succeeded() && slot != usable / util::slot_bytes_k; ++slot) {
        if (stop_requested) {
            result = stopped_recovery();
            break;
        }
        std::uint64_t const base = address.byte_base + slot * util::slot_bytes_k;
        auto [owner, owner_status] = read_owner_block(file, base);
        if (owner_status.failed()) {
            result = owner_status;
            break;
        }
        bool const held = owner.magic == util::slot_owner_magic_k &&
                          owner.format_version == util::slot_format_version_k;
        if (medium == 0 && slot == 0) {
            if (!held)
                result = {util::status_code_t::not_found_k, "The first address holds no database of this layout"};
            // The copy a slot points at repeats the slot's generation, which the publication steps.
            else recovery.manifest.id = owner.database, recovery.manifest.generation = owner.manifest_generation + 1;
            continue;
        }
        if (!held || owner.database != recovery.manifest.id) continue;
        if (owner.medium != medium) {
            result = {util::status_code_t::malformed_k, "The data directories are listed in another order"};
            break;
        }
        // Short runs share a packed slot back to back, each starting where the one before
        // it ends, aligned
        std::uint64_t offset = base + util::slot_payload_offset_k;
        while (result.succeeded() && offset + sizeof(blobs::sorted_run_header_t) <= base + util::slot_bytes_k) {
            blobs::sorted_run_header_t header {};
            auto [opens, read_status] = read_run_header(file, offset, header);
            if (read_status.failed()) result = read_status;
            if (!opens || read_status.failed()) break;
            auto [bytes, bytes_status] = run_bytes_of(header);
            if (bytes_status.failed()) {
                result = bytes_status;
                break;
            }
            // A run of no bytes recurs at the same offset, and one past the slot is another's
            if (bytes == 0 || bytes > base + util::slot_bytes_k - offset) {
                result = {util::status_code_t::malformed_k, "A run's header claims bytes its slot does not hold"};
                break;
            }
            util::manifest_file_t found;
            found.offset = offset;
            found.bytes = bytes;
            found.medium = medium;
            result = recover_run(recovery, header, found);
            offset += util::round_up_to_multiple(bytes, util::portable_alignment_k);
        }
    }
    [[maybe_unused]] util::status_t const closed = util::close_native_file(file);
    return result;
}

/** The transport recover reads runs back through, positioned reads on the calling thread. */
using recovery_disk_t = util::blocking_files_disk<fu::standard_atomic_ref, fu::standard_yield_t>;

/** What a run opened offline reads through: the disk the run's file or device is registered on. */
struct recovery_volume_t {
    using disk_t = recovery_disk_t;
    recovery_disk_t* disk_ = nullptr;
    [[nodiscard]] recovery_disk_t& disk() const noexcept { return *disk_; }
};

/** A run of the collections system collection, whose values are variable-width records. */
using collections_run_t = blobs::lsm_sorted_run<blobs::key_slot<st::key128_t, blobs::value_layout_t::variable_width_k>,
                                                recovery_volume_t, fu::standard_yield_t>;

/** The newest version of one collection's record across every run of the
 *  collections system collection. */
struct collections_version_t {
    st::key128_t key;
    std::uint64_t stamp = 0;
    util::delta_kind_t kind = util::delta_kind_t::write_k;
    st::vector<char> value;
};

/** Reads every key slot of the run at @p extent into @p newest, keeping the version of the newest
 *  stamp per key. */
util::status_t read_collections_run(recovery_volume_t& volume, util::io_channel_t& channel,
                                    util::medium_extent_t const& extent,
                                    st::vector<collections_version_t>& newest) noexcept {
    auto [run, open_status] = collections_run_t::open(volume, channel, extent);
    if (open_status.failed()) return open_status;
    st::vector<util::byte_t> value;
    for (std::uint64_t position = 0; position != run.population_count(); ++position) {
        st::key128_t const key = run.key_slot(position).stored_key;
        std::uint64_t const stamp = run.stamp(position);
        std::size_t index = 0;
        while (index != newest.size() && !(newest[index].key == key)) ++index;
        if (index != newest.size() && newest[index].stamp >= stamp) continue;
        std::uint32_t const bytes = run.value_window(position).bytes;
        if (value.resize(bytes) != st::status_t::success_k)
            return {util::status_code_t::out_of_memory_k, "No room to read a collection's record"};
        if (util::status_t const read = run.read_value(channel, position, std::span<util::byte_t> {value});
            read.failed())
            return read;
        if (index == newest.size() && newest.push_back(collections_version_t {}) != st::status_t::success_k)
            return {util::status_code_t::out_of_memory_k, "No room to keep a collection's record"};
        collections_version_t& version = newest[index];
        version.key = key, version.stamp = stamp, version.kind = run.kind(position);
        if (version.value.resize(bytes) != st::status_t::success_k)
            return {util::status_code_t::out_of_memory_k, "No room to keep a collection's record"};
        std::memcpy(version.value.data(), value.data(), bytes);
    }
    return {};
}

/** Reads the newest record of every collection from the runs of the collections system collection
 *  @p recovery found, registering each run's file, or the raw medium holding it, on one blocking
 *  disk in turn, reporting each run into @p phase and stopping between runs once a signal asks. */
util::status_t read_collections_records(recovery_t const& recovery, std::span<util::data_address_t const> addresses,
                                        st::vector<collections_version_t>& newest, verb_phase_t& phase) noexcept {
    util::resource_constraint<fu::standard_atomic_ref> budget {util::unlimited_resource_k};
    auto [device, device_status] = util::blocking_files_device_t::make(util::file_buffering_t::buffered_k);
    if (device_status.failed()) return device_status;
    util::disk_config_t config;
    auto [disk, disk_status] = recovery_disk_t::make(std::move(device), config, budget);
    if (disk_status.failed()) return disk_status;
    util::io_channel_config_t const channels_config;
    std::array<fu_memory_domain_id_t, 1> const anywhere {-1};
    auto [queues, queues_status] = util::nvme_queues_t::make(util::io_transport_t::blocking_k, channels_config,
                                                             anywhere);
    util::status_t result = queues_status;
    recovery_volume_t volume {&disk};
    for (std::size_t index = 0; result.succeeded() && index != recovery.runs.size(); ++index) {
        if (verb_progress(index, recovery.runs.size(), &phase) == ustore_cancel_k) {
            result = stopped_recovery();
            break;
        }
        found_run_t const& found = recovery.runs[index];
        if (found.header.collection != util::collections_collection_uuid_k) continue;
        char path[util::path_max_bytes_k];
        util::data_address_t const& address = addresses[found.file.medium];
        if (recovery.manifest.placement == util::manifest_placement_t::paths_k) {
            char directory[util::path_max_bytes_k], name[util::path_max_bytes_k];
            result = terminated(address.location, directory);
            if (result.succeeded()) result = recovery.manifest.file_name(found.file.run, name);
            if (result.succeeded()) result = util::join_path(directory, name, path);
        }
        else result = terminated(address.location, path);
        if (result.failed()) break;
        util::medium_options_t request;
        request.name = path;
        request.access = util::medium_access_t::read_only_k;
        auto [medium, register_status] = disk.register_medium(request);
        if ((result = register_status).failed()) break;
        auto [channel, media_status] = queues.reading_channel_with_disk_media(0, disk);
        result = media_status;
        if (result.succeeded()) {
            util::medium_extent_t extent;
            extent.medium = medium.reference();
            extent.extent = util::extent_t {found.file.offset,
                                            util::round_up_to_multiple(found.file.bytes, util::portable_alignment_k)};
            result = read_collections_run(volume, *channel, extent, newest);
            [[maybe_unused]] util::status_t const drained = disk.drain(channel->queue_pair());
        }
        if (util::status_t const closed = disk.unregister_medium(medium); closed.failed() && result.succeeded())
            result = closed;
    }
    return result;
}

/** The record of the reserved collection a run of @p header belongs to, as the engine's
 *  open creates it. */
[[nodiscard]] util::manifest_collection_t reserved_record(blobs::sorted_run_header_t const& header) noexcept {
    util::manifest_collection_t record = util::default_reserved_record(header.collection, header.value_max_size);
    ustore_modality_t const modality = header.collection == util::names_collection_uuid_k ? ustore_modality_strings_k
                                                                                          : ustore_modality_blobs_k;
    // A store tag keeps the modality in its high byte and the value layout in its low one, as the
    // engine persists it.
    record.store = static_cast<std::uint16_t>(static_cast<unsigned>(modality) << 8 |
                                              static_cast<unsigned>(header.value_layout));
    return record;
}

/**
 *  @brief Rebuilds every collection the newest records of the collections system collection list,
 *      and points every run @p recovery found at its collection.
 *
 *  The two system collections take the records the engine opens them with, when runs of theirs were
 *  found. A run of a collection the records list as dropped, which a crash left behind, or never
 *  list at all is left out with a note. Each run read is reported into @p phase.
 */
util::status_t assign_collections(recovery_t& recovery, std::span<util::data_address_t const> addresses,
                                  verb_phase_t& phase) noexcept {
    st::vector<collections_version_t> newest;
    if (util::status_t const read = read_collections_records(recovery, addresses, newest, phase); read.failed())
        return read;
    util::manifest_t& manifest = recovery.manifest;
    for (found_run_t const& found : recovery.runs) {
        util::uuid128_t const& id = found.header.collection;
        if (id != util::collections_collection_uuid_k && id != util::names_collection_uuid_k) continue;
        std::size_t index = 0;
        while (index != manifest.collections.size() && manifest.collections[index].id != id) ++index;
        if (index != manifest.collections.size()) continue;
        if (manifest.collections.push_back(reserved_record(found.header)) != st::status_t::success_k)
            return {util::status_code_t::out_of_memory_k, "No room to record a recovered collection"};
    }
    for (collections_version_t const& version : newest) {
        if (version.kind != util::delta_kind_t::write_k) continue;
        if (version.value.size() < sizeof(util::collections_record_t))
            return {util::status_code_t::malformed_k, "A collection's record is shorter than its fields"};
        util::collections_record_t fields;
        std::memcpy(&fields, version.value.data(), sizeof(fields));
        util::manifest_collection_t record;
        version.key.to_bytes(
            std::as_writable_bytes(std::span<std::uint8_t, util::uuid128_t::size_bytes_k> {record.id.bytes}));
        // The engine names a collection in the manifest by its printed UUID, its own name living in
        // the record alone
        char printed[util::uuid128_t::printed_length_k];
        record.id.print(printed);
        if (sz::failed(record.name.assign(util::string_view_t {printed, sizeof(printed)})))
            return {util::status_code_t::out_of_memory_k, "No room to name a recovered collection"};
        record.store = fields.store_tag;
        record.value_size = fields.value_size;
        record.value_max_size = fields.value_max_size;
        record.level_enlarge_factor = fields.level_enlarge_factor;
        record.cache_limit = fields.cache_limit;
        record.memory_runs_max_bytes = fields.memory_runs_max_bytes;
        record.memory_runs_max_deltas = fields.memory_runs_max_deltas;
        if (manifest.collections.push_back(std::move(record)) != st::status_t::success_k)
            return {util::status_code_t::out_of_memory_k, "No room to record a recovered collection"};
    }
    if (manifest.files.reserve(recovery.runs.size()) != st::status_t::success_k)
        return {util::status_code_t::out_of_memory_k, "No room to record the recovered runs"};
    for (found_run_t const& found : recovery.runs) {
        std::size_t index = 0;
        while (index != manifest.collections.size() && manifest.collections[index].id != found.header.collection)
            ++index;
        if (index == manifest.collections.size()) {
            char printed[util::uuid128_t::printed_length_k];
            found.file.run.print(printed);
            fmt::println(stderr, style_of(tone_t::failure_k, stderr),
                         "Leaving out run {}, whose collection the records list as dropped or never named",
                         std::string_view {printed, sizeof(printed)});
            continue;
        }
        util::manifest_file_t file = found.file;
        file.collection = static_cast<std::uint32_t>(index);
        [[maybe_unused]] st::status_t const placed = manifest.files.push_back(st::assume_reserved, std::move(file));
    }
    return {};
}

/** Writes @p json into the idle region of the manifest slot at @p address and points the slot's
 *  owner block at it, flushing after each step, as a slotted volume publishes. */
util::status_t publish_raw_manifest(util::data_address_t const& address, std::span<char const> json) noexcept {
    if (json.size() > manifest_region_bytes_k)
        return {util::status_code_t::capacity_exhausted_k, "The manifest outgrew its region of the manifest slot"};
    char path[util::path_max_bytes_k];
    if (util::status_t const copied = terminated(address.location, path); copied.failed()) return copied;
    auto [file, open_status] = util::open_native_file(path, util::file_access_t::read_write_k,
                                                      util::file_buffering_t::buffered_k);
    if (open_status.failed()) return open_status;
    util::status_t result;
    auto [owner, owner_status] = read_owner_block(file, address.byte_base);
    std::uint64_t const regions = address.byte_base + util::slot_payload_offset_k;
    std::uint64_t const idle = owner.manifest_offset == regions ? regions + manifest_region_bytes_k : regions;
    if (owner_status.failed()) result = owner_status;
    else result = write_exactly(file, idle, std::as_bytes(json));
    // The copy ends at its first zero byte too, so a zero follows it whenever the region has room.
    util::byte_t const terminator[1] {};
    if (result.succeeded() && json.size() < manifest_region_bytes_k)
        result = write_exactly(file, idle + json.size(), terminator);
    if (result.succeeded()) result = util::flush_native_file(file);
    if (result.succeeded()) {
        owner.manifest_generation += 1;
        owner.manifest_offset = idle;
        owner.manifest_bytes = json.size();
        result = write_exactly(file, address.byte_base, std::as_bytes(std::span {&owner, 1}));
    }
    if (result.succeeded()) result = util::flush_native_file(file);
    [[maybe_unused]] util::status_t const closed = util::close_native_file(file);
    return result;
}

/** Rebuilds the manifest of the database @p line names from its runs, every one on level 0,
 *  and publishes it; a stop before the publication writes nothing. */
[[nodiscard]] bool recover_database(command_line_t const& line) noexcept {
    recovery_t recovery;
    util::manifest_t& manifest = recovery.manifest;
    st::vector<util::data_address_t> addresses;
    util::status_t status;
    if (line.addresses.size() > (std::numeric_limits<std::uint16_t>::max)())
        status = {util::status_code_t::invalid_argument_k, "A database spans at most 65,535 data directories"};
    else if (sz::failed(manifest.database.assign(util::string_view_t {line.database})) ||
             addresses.reserve(line.addresses.size()) != st::status_t::success_k)
        status = {util::status_code_t::out_of_memory_k, "No room for the data directories"};
    for (std::size_t index = 0; status.succeeded() && index != line.addresses.size(); ++index) {
        auto [parsed, parse_status] = util::parse_data_address(line.addresses[index]);
        if (parse_status.failed()) status = parse_status;
        else if (parsed.kind == util::data_address_kind_t::spdk_k)
            status = {util::status_code_t::unsupported_k, "SPDK namespaces are recovered through a mounted database"};
        else if (index != 0 && parsed.kind != addresses[0].kind)
            status = {util::status_code_t::invalid_argument_k, "The data directories are of one kind"};
        else [[maybe_unused]]
            st::status_t const placed = addresses.push_back(st::assume_reserved, util::data_address_t {parsed});
    }
    bool const raw = status.succeeded() && addresses[0].kind == util::data_address_kind_t::raw_k;
    manifest.placement = raw ? util::manifest_placement_t::offsets_k : util::manifest_placement_t::paths_k;
    char path[util::path_max_bytes_k];
    if (status.succeeded() && !raw) {
        status = manifest_path(addresses[0].location, line.database, path);
        st::vector<char> existing;
        util::status_t const found = status.succeeded() ? util::read_file(path, existing) : status;
        if (status.succeeded() && found != util::status_code_t::not_found_k)
            status = found.failed() ? found
                                    : util::status_t {util::status_code_t::already_exists_k,
                                                      "The database has a manifest; move it away to recover"};
        manifest.id = util::uuid_generator();
    }
    verb_phase_t scan {.name = "scan", .unit = "data directories"};
    verb_phase_t read {.name = "read", .unit = "runs"};
    for (std::size_t index = 0; status.succeeded() && index != addresses.size(); ++index) {
        if (verb_progress(index, addresses.size(), &scan) == ustore_cancel_k) status = stopped_recovery();
        else if (raw) status = recover_raw(recovery, addresses[index], static_cast<std::uint16_t>(index));
        else status = recover_directory(recovery, addresses[index].location, static_cast<std::uint16_t>(index));
    }
    if (status == util::status_code_t::cancelled_k) {
        verb_stopped(scan);
        return true;
    }
    if (status.succeeded())
        status = assign_collections(recovery, std::span<util::data_address_t const> {addresses}, read);
    if (status == util::status_code_t::cancelled_k) {
        verb_stopped(read);
        return true;
    }

    st::vector<char> json;
    if (status.succeeded()) status = manifest.print(st::default_allocator<util::byte_t> {}, json);
    if (status.succeeded())
        status = raw ? publish_raw_manifest(addresses[0], std::span<char const> {json})
                     : util::replace_file_durably(path, std::span<char const> {json});
    if (status.failed()) {
        print_failure("Failed to recover the database", status);
        return false;
    }
    fmt::println(stderr, style_of(tone_t::success_k, stderr),
                 "Recovered {} runs of {} collections, every run on level 0", manifest.files.size(),
                 manifest.collections.size());
    return true;
}

#pragma endregion Recovery

/** Runs the command of @p line and answers whether it succeeded. */
[[nodiscard]] bool execute(ustore_database_t database, ustore_executor_t executor,
                           command_line_t const& line) noexcept {
    switch (line.command) {
    case command_t::none_k: return false;
    case command_t::collection_create_k: return create_collection(database, executor, line);
    case command_t::collection_drop_k: return drop_collection(database, executor, line.collection);
    case command_t::collection_list_k: return list_collections(database, executor);
    case command_t::collection_import_k:
        return is_text(line.format_of(line.input)) ? import_documents(database, executor, line)
                                                   : import_arrow(database, executor, line);
    case command_t::collection_export_k:
        return is_text(line.format_of(line.output)) ? export_documents(database, executor, line)
                                                    : export_arrow(database, executor, line);
    case command_t::snapshot_save_k: return save_snapshot(database, executor, line.snapshot_path);
    case command_t::snapshot_export_k: return export_snapshot(database, executor, line.snapshot_path);
    case command_t::snapshot_import_k: return import_snapshot(database, executor, line.snapshot_path);
    case command_t::manifest_print_k: return print_manifest(line);
    case command_t::database_recover_k: return recover_database(line);
    }
    return false;
}

#pragma endregion Commands

} // namespace unum::ustore::tools

int main(int argc, char* argv[]) {
    using namespace unum::ustore;
    using namespace unum::ustore::tools;

    command_line_t line;
    std::array<flag_t, 16> const flags {{
        {"--directory", "path", "database directory, required", &line.directory},
        {"--engine", "name", "engine to open: ram, rocksdb, leveldb or nvme", &line.engine.name},
        {"--isolation", "name", "isolation of ram: strict-serializable or snapshot", &line.engine.isolation},
        {"--engine-transport", "name", "transport of nvme, like blocking or linux-uring", &line.engine.transport},
        {"--threads", "count", "threads every call runs on, 0 for the cores of the affinity mask",
         &line.engine.threads},
        {"--collection", "name", "collection a command acts on", &line.collection},
        {"--input", "files", "files a collection import reads, comma-separated, or '-' for standard input",
         &line.input},
        {"--output", "file", "file a collection export writes, or '-' for standard output", &line.output},
        {"--id-field", "field", "field holding the keys of imported documents", &line.id_field},
        {"--memory-limit", "bytes", "bytes an import or export buffers per batch, like 64M", &line.memory_limit},
        {"--modality", "name", "what a created collection holds: blobs, docs, graph, strings or pages", &line.modality},
        {"--format", "name", "automatic, ndjson, csv, arrows or arrow, where '-' is sniffed", &line.format},
        {"--ingest", "order", "how an Arrow import lands: batched, sorted or unsorted", &line.ingest},
        {"--alignment", "bytes", "Arrow buffer alignment of an export: 64 or 4096", &line.alignment},
        {"--batch-rows", "count", "most rows per Arrow batch of an export", &line.batch_rows},
        {"--database", "name", "NVMe database a manifest or a recovery names", &line.database},
    }};

    char synopsis[4096 + commands_k.size()];
    auto const written = fmt::format_to_n(synopsis, sizeof(synopsis) - 1,
                                          "{} --directory <path> <command> [flags]\n\n{}", argv[0], commands_k);
    *written.out = '\0';

    for (flag_t const& flag : flags) {
        sz::string_t const variable = ustore_variable(flag);
        char const* const value = std::getenv(variable.c_str());
        if (!value || !*value || assign_variable_(flag, value) == argument_status_t::parsed_k) continue;
        fmt::println(stderr, "{}=\"{}\" does not parse, expected {}", variable, value, grammar(flag));
        return 1;
    }
    // A recovery takes every data directory as a positional, so there is room for every argument
    st::vector<std::string_view> positionals;
    if (positionals.resize(static_cast<std::size_t>(argc)) != st::status_t::success_k) return 1;
    arguments_t const parsed = parse_arguments(argc, argv, flags, std::span<std::string_view> {positionals});
    if (parsed.status == argument_status_t::help_k) {
        print_usage(synopsis, flags);
        return 0;
    }
    if (parsed.status != argument_status_t::parsed_k) {
        if (!parsed.flag) fmt::println(stderr, "{} is not a flag, see --help", parsed.argument);
        else
            fmt::println(stderr, "{}=\"{}\" does not parse, expected {}", parsed.flag->name, parsed.value,
                         grammar(*parsed.flag));
        return 1;
    }
    if (!names_parse(line)) return 1;
    char const* problem = parse_command({positionals.data(), parsed.positionals_count}, line);
    if (!problem && line.directory.empty() && !line.opens_no_engine()) problem = "The --directory flag is required";
    if (problem) {
        fmt::println(stderr, "{}, see --help", problem);
        return 1;
    }

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);
    bool succeeded = false;
    if (line.opens_no_engine()) succeeded = execute(nullptr, nullptr, line);
    else {
        ustore_executor_t executor = nullptr;
        ustore_database_t database = nullptr;
        ustore_outcome_t outcome {};
        line.engine.locate(line.directory.data());
        spawn_executor(line.engine, &executor, &outcome);
        if (!failed(outcome)) open_engine(line.engine, executor, &database, &outcome);
        if (failed(outcome)) {
            print_failure("Failed to open the database", outcome);
            ustore_executor_free(executor);
            return 1;
        }
        succeeded = execute(database, executor, line);
        // Freeing writes nothing, so the commits of a changing command reach the disk here
        if (succeeded && line.writes()) {
            ustore_database_flush_t flush {.database = database, .executor = executor, .outcome = &outcome};
            ustore_database_flush(&flush);
            if (failed(outcome)) print_failure("Failed to flush the database", outcome);
            succeeded = !failed(outcome);
        }
        ustore_database_free(database);
        ustore_executor_free(executor);
    }
    if (!succeeded) return 1;
    return stop_requested ? exit_stopped_k : 0;
}
