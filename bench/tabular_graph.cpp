/**
 *  @file bench/tabular_graph.cpp
 *  @author Ash Vardanian
 *  @date October 2, 2022
 *  @brief Benchmarks importing NDJSON or CSV files as documents and as a graph, and exporting both back.
 *
 *  Every row becomes one document and one edge between the integer source and target columns,
 *  labeled by the edge column.
 *
 *  Beside the flags of @c bench/harness.hpp, it takes:
 *
 *  Flag                       Default                Meaning
 *  `--dataset`                required               Directory of input files
 *  `--format`                 `ndjson`               Format of the inputs and the exports, or `csv`
 *  `--directory`              required               Directory of the exports and the database
 *  `--engine`                 `ram`                  Engine to open
 *  `--isolation`              `strict-serializable`  Isolation of the transactions of `ram`
 *  `--engine-transport`       `automatic`            Transport of `nvme`
 *  `--id-field`               required               Field holding each document's key
 *  `--source-field`           required               Integer field holding the source vertex
 *  `--target-field`           required               Integer field holding the target vertex
 *  `--edge-field`             required               Field holding the edge key
 *  `--threads`                `1`                    Threads every row runs on
 *  `--max-input-files-total`  `10`                   Input files listed from the dataset
 *  `--memory-limit`           `1GB`                  Bytes an import or export buffers per batch
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uintmax_t`
#include <cstdio>  // `stderr`

#include <array>            // `std::array`
#include <initializer_list> // `std::initializer_list`
#include <filesystem>       // `std::filesystem::directory_iterator`, `std::filesystem::file_size`
#include <string>           // `std::string`, what `std::filesystem::path::string` answers, and the row names
#include <string_view>      // `std::string_view`
#include <system_error>     // `std::error_code`

#include <fmt/format.h>

#include <smashtable/basic_vector.hpp>
#include <stringzilla/stringzilla.hpp>

#include "ustore/blobs.h"
#include "ustore/db.h"
#include "ustore/types.hpp"

#include "dataset.h"
#include "harness.hpp"

namespace unum::ustore::bench {

/** The format of the input files, which the exports also take, in the order of @c format_names_k. */
enum class file_format_t : std::size_t {
    ndjson_k,
    csv_k,
};

/** The names @c --format accepts, which are also the file extensions without the dot. */
constexpr std::array<std::string_view, 2> format_names_k {"ndjson", "csv"};

/** The name and file extension of @p format. */
[[nodiscard]] constexpr std::string_view format_as(file_format_t format) noexcept {
    return format_names_k[static_cast<std::size_t>(format)];
}

/** Command-line options of the benchmark, each field set by the flag of the same name; text views into @c argv. */
struct settings_t : shared_settings_t {

    /** Directory holding the input files. */
    std::string_view dataset;

    /** Format of the input files and the exports. */
    choice_t format {format_names_k};

    /** Directory the exports are written to and the database lives in. */
    std::string_view directory;

    /** The engine to open and the options of its init call. */
    engine_options_t engine;

    /** Field holding the document keys. */
    std::string_view id_field;

    /** Field holding the source vertices. */
    std::string_view source_field;

    /** Field holding the target vertices. */
    std::string_view target_field;

    /** Field holding the edge keys. */
    std::string_view edge_field;

    /** Threads every benchmark runs on. */
    count_t threads {.value = 1, .minimum = 1};

    /** Input files listed from the dataset, divided into one contiguous share per thread. */
    count_t max_input_files_total {.value = 10, .minimum = 1};

    /** Bytes an import or export buffers per batch. */
    bytes_t memory_limit {std::size_t {1} << 30};
};

/** The input and export format @p settings selected, which @c format_names_k orders. */
[[nodiscard]] file_format_t selected_format(settings_t const& settings) noexcept {
    return static_cast<file_format_t>(settings.format.selected);
}

/** One input file, whose path lives in the shared paths buffer. */
struct source_file_t {

    /** Offset of the file's null-terminated path inside @c tabular_t::paths. */
    std::size_t path_offset;

    /** Bytes the file holds. */
    std::size_t size;
};

/** The open database and the input files every benchmark shares. */
struct tabular_t {

    /** Open database. */
    ustore_database_t db = nullptr;

    /** The graph collection the graph benchmarks import into and export from. */
    ustore_collection_id_t graph = ustore_collection_main_k;

    /** Null-terminated paths of the input files, one after another. */
    sz::string_t paths;

    /** The listed input files, whose shares the import threads take. */
    st::basic_vector<source_file_t> files;

    /** The null-terminated path of the input file at @p index. */
    [[nodiscard]] char const* path(std::size_t index) const noexcept { return paths.data() + files[index].path_offset; }
};

/** Remembers the path and size of @p name under @p directory, answering whether both fit. */
[[nodiscard]] bool remember_file(tabular_t& tabular, sz::string_t const& directory, char const* name) noexcept {
    std::size_t const offset = tabular.paths.size();
    if (!tabular.paths.try_append(directory) || !tabular.paths.try_append(name) || !tabular.paths.try_push_back('\0'))
        return false;
    std::error_code error;
    std::uintmax_t const size = std::filesystem::file_size(tabular.paths.data() + offset, error);
    if (error) return true;
    source_file_t const file {offset, static_cast<std::size_t>(size)};
    return st::succeeded(tabular.files.push_back(source_file_t {file}));
}

/** Lists up to @c max_input_files_total files with the extension of the requested format. */
[[nodiscard]] bool list_files(settings_t const& settings, tabular_t& tabular) noexcept {
    std::string_view const extension = format_as(selected_format(settings));
    fmt::println("Will search for .{} files...", extension);
    sz::string_t directory;
    if (!directory.try_append(settings.dataset.data(), settings.dataset.size()) || !directory.try_push_back('/'))
        return false;

    std::error_code error;
    std::filesystem::directory_iterator listing {std::filesystem::path {settings.dataset}, error};
    if (error) {
        fmt::println(stderr, "Failed to open {}", directory.c_str());
        return false;
    }
    std::size_t const wanted = settings.max_input_files_total.value;
    bool listed = true;
    for (; !error && listed && tabular.files.size() != wanted && listing != std::filesystem::directory_iterator {};
         listing.increment(error)) {
        std::string const name = listing->path().filename().string();
        if (!std::string_view {name}.ends_with(extension) ||
            !std::string_view {name}.substr(0, name.size() - extension.size()).ends_with('.'))
            continue;
        listed = remember_file(tabular, directory, name.c_str());
    }
    if (!listed || error) {
        fmt::println(stderr, "Failed to list the files in {}", directory.c_str());
        return false;
    }
    fmt::println("Files are ready for benchmark");
    return true;
}

/** Drops the keys and values of @p collection, reporting a failure on the standard error. */
void clear_collection(ustore_database_t db, ustore_collection_id_t collection) noexcept {
    ustore_error_t error {};
    ustore_collection_drop_t drop {};
    drop.db = db;
    drop.error = &error;
    drop.collection = collection;
    drop.mode = ustore_drop_keys_vals_k;
    ustore_collection_drop(&drop);
    if (error.status) fmt::println(stderr, "Failed to clear a collection: {}", error.message);
}

/** Answers the size of the exported file at @p path and removes it, or zero when it is missing. */
[[nodiscard]] std::size_t take_exported(char const* path) noexcept {
    std::error_code error;
    std::uintmax_t const size = std::filesystem::file_size(path, error);
    if (error) return 0;
    if (!std::filesystem::remove(path, error)) fmt::println(stderr, "Failed to remove {}: {}", path, error.message());
    return static_cast<std::size_t>(size);
}

/** Composes the path of the export named @p stem that the thread at @p thread_index writes, in the input format. */
[[nodiscard]] bool export_path(sz::string_t& path, settings_t const& settings, std::string_view stem,
                               std::size_t thread_index) noexcept {
    std::string_view const extension = format_as(selected_format(settings));
    char infix[24];
    auto const written = fmt::format_to_n(infix, sizeof(infix) - 1, "-{}.", thread_index);
    *written.out = '\0';
    return path.try_append(settings.directory.data(), settings.directory.size()) && path.try_push_back('/') &&
           path.try_append(stem.data(), stem.size()) && path.try_append(infix) &&
           path.try_append(extension.data(), extension.size());
}

/** Imports every listed file as documents, each thread over its own contiguous share of the files. */
void bench_docs_import(executor_t& executor, tabular_t const& tabular, settings_t const& settings,
                       std::string_view name) {
    print(run_pass(executor, name, tabular.files.size(), [&](loop_t& loop, std::size_t) noexcept {
        ustore_arena_t arena = nullptr;
        ustore_error_t error {};
        for (std::size_t file : loop) {
            ustore_docs_import_t importing {};
            importing.db = tabular.db;
            importing.error = &error;
            importing.arena = &arena;
            importing.collection = ustore_collection_main_k;
            importing.path = tabular.path(file);
            importing.max_batch_size = settings.memory_limit.value;
            importing.id_field = settings.id_field.data();
            ustore_docs_import(&importing);
            if (error.status) loop.fail();
            else loop.count(1, tabular.files[file].size);
            error = {};
        }
        ustore_arena_free(arena);
    }));
}

/** Imports every listed file as a graph, each thread over its own contiguous share of the files. */
void bench_graph_import(executor_t& executor, tabular_t const& tabular, settings_t const& settings,
                        std::string_view name) {
    print(run_pass(executor, name, tabular.files.size(), [&](loop_t& loop, std::size_t) noexcept {
        ustore_arena_t arena = nullptr;
        ustore_error_t error {};
        for (std::size_t file : loop) {
            ustore_graph_import_t importing {};
            importing.db = tabular.db;
            importing.error = &error;
            importing.arena = &arena;
            importing.collection = tabular.graph;
            importing.path = tabular.path(file);
            importing.max_batch_size = settings.memory_limit.value;
            importing.source_id_field = settings.source_field.data();
            importing.target_id_field = settings.target_field.data();
            importing.edge_id_field = settings.edge_field.data();
            ustore_graph_import(&importing);
            if (error.status) loop.fail();
            else loop.count(1, tabular.files[file].size);
            error = {};
        }
        ustore_arena_free(arena);
    }));
}

/** Exports the documents once from every thread, each into its own file. */
void bench_docs_export(executor_t& executor, tabular_t const& tabular, settings_t const& settings,
                       std::string_view name) {
    print(run_pass(executor, name, executor.threads_count(), [&](loop_t& loop, std::size_t thread) noexcept {
        sz::string_t output;
        if (!export_path(output, settings, "tabular_graph_docs", thread))
            return loop.skip("no memory for the output path");
        ustore_arena_t arena = nullptr;
        ustore_error_t error {};
        for ([[maybe_unused]] std::size_t call : loop) {
            ustore_docs_export_t exporting {};
            exporting.db = tabular.db;
            exporting.error = &error;
            exporting.arena = &arena;
            exporting.collection = ustore_collection_main_k;
            exporting.path = output.c_str();
            exporting.max_batch_size = settings.memory_limit.value;
            ustore_docs_export(&exporting);
            if (error.status) loop.fail();
            else loop.count(1, take_exported(output.c_str()));
            error = {};
        }
        ustore_arena_free(arena);
    }));
}

/** Exports the graph once from every thread, each into its own file. */
void bench_graph_export(executor_t& executor, tabular_t const& tabular, settings_t const& settings,
                        std::string_view name) {
    print(run_pass(executor, name, executor.threads_count(), [&](loop_t& loop, std::size_t thread) noexcept {
        sz::string_t output;
        sz::string_t source;
        sz::string_t target;
        sz::string_t edge;
        if (!export_path(output, settings, "tabular_graph_edges", thread) ||
            !source.try_append(settings.source_field.data(), settings.source_field.size()) ||
            !target.try_append(settings.target_field.data(), settings.target_field.size()) ||
            !edge.try_append(settings.edge_field.data(), settings.edge_field.size()))
            return loop.skip("no memory for the export names");
        // Exported column names cannot hold the slashes of JSON pointers.
        for (sz::string_t* name : {&source, &target, &edge})
            for (std::size_t index = 0; index != name->size(); ++index)
                if ((*name)[index] == '/') (*name)[index] = '_';

        ustore_arena_t arena = nullptr;
        ustore_error_t error {};
        for ([[maybe_unused]] std::size_t call : loop) {
            ustore_graph_export_t exporting {};
            exporting.db = tabular.db;
            exporting.error = &error;
            exporting.arena = &arena;
            exporting.collection = tabular.graph;
            exporting.path = output.c_str();
            exporting.max_batch_size = settings.memory_limit.value;
            exporting.source_id_field = source.c_str();
            exporting.target_id_field = target.c_str();
            exporting.edge_id_field = edge.c_str();
            ustore_graph_export(&exporting);
            if (error.status) loop.fail();
            else loop.count(1, take_exported(output.c_str()));
            error = {};
        }
        ustore_arena_free(arena);
    }));
}

} // namespace unum::ustore::bench

int main(int argc, char** argv) {
    using namespace unum::ustore;
    using namespace unum::ustore::bench;

    settings_t settings;
    std::array<flag_t, 13> const flags {{
        {"--dataset", "path", "directory of input files, required", &settings.dataset},
        {"--format", "name", "format of the input files and the exports", &settings.format},
        {"--engine", "name", "engine to open: ram, rocksdb, leveldb or nvme", &settings.engine.name},
        {"--isolation", "name", "isolation of the transactions of ram", &settings.engine.isolation},
        {"--engine-transport", "name", "transport of nvme", &settings.engine.transport},
        {"--directory", "path", "directory of the exports and of the database, required", &settings.directory},
        {"--id-field", "field", "field holding each document's key, required", &settings.id_field},
        {"--source-field", "field", "integer field holding the source vertex, required", &settings.source_field},
        {"--target-field", "field", "integer field holding the target vertex, required", &settings.target_field},
        {"--edge-field", "field", "field holding the edge key, required", &settings.edge_field},
        {"--threads", "count", "threads every benchmark runs on", &settings.threads},
        {"--max-input-files-total", "count", "input files listed from the dataset", &settings.max_input_files_total},
        {"--memory-limit", "size", "bytes an import or export buffers per batch, like 1GB", &settings.memory_limit},
    }};

    read_settings(argc, argv, settings, flags);
    if (settings.dataset.empty() || settings.directory.empty() || settings.id_field.empty() ||
        settings.source_field.empty() || settings.target_field.empty() || settings.edge_field.empty()) {
        fmt::println(stderr,
                     "Required: --dataset, --directory, --id-field, --source-field, --target-field and --edge-field");
        return 1;
    }
    if (settings.max_input_files_total.value < settings.threads.value) {
        fmt::println(stderr, "--max-input-files-total must be at least --threads");
        return 1;
    }
    tabular_t tabular;
    if (!list_files(settings, tabular)) return 1;

    ustore_error_t error {};
    settings.engine.directory = settings.directory.data();
    open_engine(settings.engine, &tabular.db, &error);
    if (error.status) {
        fmt::println(stderr, "Failed to open the database: {}", error.message);
        return 1;
    }
    ustore_collection_create_t create {};
    create.db = tabular.db;
    create.error = &error;
    create.modality = ustore_modality_graph_k;
    create.collection = &tabular.graph;
    ustore_collection_create(&create);
    if (error.status) {
        fmt::println(stderr, "Failed to create the graph collection: {}", error.message);
        ustore_database_free(tabular.db);
        return 1;
    }

    auto maybe_executor = executor_t::make(settings.threads.value, caller_exclusivity_t::caller_inclusive_k);
    if (!maybe_executor) {
        fmt::println(stderr, "Failed to spawn {} threads", settings.threads.value);
        ustore_database_free(tabular.db);
        return 1;
    }
    executor_t& executor = *maybe_executor;
    std::string_view const format = format_as(selected_format(settings));
    std::string const docs_import_name = fmt::format("docs_import_{}", format);
    std::string const docs_export_name = fmt::format("docs_export_{}", format);
    std::string const graph_import_name = fmt::format("graph_import_{}", format);
    std::string const graph_export_name = fmt::format("graph_export_{}", format);
    if (settings.selects(docs_import_name)) bench_docs_import(executor, tabular, settings, docs_import_name);
    if (settings.selects(docs_export_name)) bench_docs_export(executor, tabular, settings, docs_export_name);
    clear_collection(tabular.db, ustore_collection_main_k);
    if (settings.selects(graph_import_name)) bench_graph_import(executor, tabular, settings, graph_import_name);
    if (settings.selects(graph_export_name)) bench_graph_export(executor, tabular, settings, graph_export_name);
    clear_collection(tabular.db, tabular.graph);

    ustore_database_free(tabular.db);
    return 0;
}
