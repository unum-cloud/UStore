/**
 *  @file bench/tabular_graph.cpp
 *  @author Ash Vardanian
 *  @date October 2, 2022
 *  @brief Benchmarks importing NDJSON or CSV files as documents and as a graph, and exporting both back.
 *
 *  Every row becomes one document and one edge between the integer source and target columns,
 *  labeled by the edge column.
 */

#include <cstddef>    // `std::size_t`
#include <cstdio>     // `stderr`
#include <dirent.h>   // `opendir`, `readdir`, `closedir`
#include <fcntl.h>    // `open`
#include <sys/stat.h> // `fstat`, `stat`
#include <unistd.h>   // `close`, `read`, `unlink`

#include <array>            // `std::array`
#include <chrono>           // `std::chrono::steady_clock`
#include <initializer_list> // `std::initializer_list`
#include <limits>           // `std::numeric_limits`
#include <string_view>      // `std::string_view`

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#include <smashtable/basic_vector.hpp>
#include <stringzilla/stringzilla.hpp>

#include <ustore/blobs.h>
#include <ustore/db.h>
#include <ustore/types.hpp>

#include "arguments.hpp"
#include "dataset.h"

namespace bm = benchmark;

namespace unum::ustore::bench {

/** The format of the input files, which the exports also take, in the order of @c format_names_k. */
enum class file_format_t : std::size_t {
    ndjson_k,
    csv_k,
};

/** The names @c --format accepts, which are also the file extensions without the dot. */
constexpr std::array<std::string_view, 2> format_names_k {"ndjson", "csv"};

/** The name and file extension of @p format. */
[[nodiscard]] constexpr std::string_view format_name(file_format_t format) noexcept {
    return format_names_k[static_cast<std::size_t>(format)];
}

/** Command-line options of the benchmark, each field set by the flag of the same name; text views into @c argv. */
struct settings_t {

    /** Directory holding the input files. */
    std::string_view dataset;

    /** Format of the input files and the exports. */
    choice_t format {format_names_k};

    /** File holding the database configuration as JSON; without it the database lives in @c directory. */
    std::string_view config;

    /** Directory the exports are written to, and the database lives in without @c config. */
    std::string_view directory;

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

    /** Null-terminated paths of the input files, one after another. */
    sz::string_t paths;

    /** The listed input files, whose shares the import threads take. */
    st::basic_vector<source_file_t> files;

    /** The null-terminated path of the input file at @p index. */
    [[nodiscard]] char const* path(std::size_t index) const noexcept { return paths.data() + files[index].path_offset; }
};

/** Reads the whole file at @p path into @p content, answering whether it succeeded. */
[[nodiscard]] bool read_file(char const* path, sz::string_t& content) noexcept {
    int const descriptor = open(path, O_RDONLY);
    if (descriptor == -1) return false;
    struct stat metadata {};
    bool loaded = fstat(descriptor, &metadata) == 0 && content.try_resize(static_cast<std::size_t>(metadata.st_size));
    std::size_t filled = 0;
    while (loaded && filled != content.size()) {
        ssize_t const got = read(descriptor, content.data() + filled, content.size() - filled);
        loaded = got > 0;
        filled += loaded ? static_cast<std::size_t>(got) : 0;
    }
    [[maybe_unused]] int const closed = close(descriptor);
    return loaded;
}

/** Remembers the path and size of @p name under @p directory, answering whether both fit. */
[[nodiscard]] bool remember_file(tabular_t& tabular, sz::string_t const& directory, char const* name) noexcept {
    std::size_t const offset = tabular.paths.size();
    if (!tabular.paths.try_append(directory) || !tabular.paths.try_append(name) || !tabular.paths.try_push_back('\0'))
        return false;
    struct stat metadata {};
    if (stat(tabular.paths.data() + offset, &metadata) != 0) return true;
    source_file_t const file {offset, static_cast<std::size_t>(metadata.st_size)};
    return st::succeeded(tabular.files.push_back(source_file_t {file}));
}

/** Lists up to @c max_input_files_total files with the extension of the requested format. */
[[nodiscard]] bool list_files(settings_t const& settings, tabular_t& tabular) noexcept {
    std::string_view const extension = format_name(selected_format(settings));
    fmt::println("Will search for .{} files...", extension);
    sz::string_t directory;
    if (!directory.try_append(settings.dataset.data(), settings.dataset.size()) || !directory.try_push_back('/'))
        return false;

    DIR* const listing = opendir(directory.c_str());
    if (!listing) {
        fmt::println(stderr, "Failed to open {}", directory.c_str());
        return false;
    }
    std::size_t const wanted = settings.max_input_files_total.value;
    bool listed = true;
    for (dirent* entry = readdir(listing); entry && listed && tabular.files.size() != wanted;
         entry = readdir(listing)) {
        std::string_view const name {entry->d_name};
        if (!name.ends_with(extension) || !name.substr(0, name.size() - extension.size()).ends_with('.')) continue;
        listed = remember_file(tabular, directory, entry->d_name);
    }
    [[maybe_unused]] int const closed = closedir(listing);
    if (!listed) {
        fmt::println(stderr, "Failed to list the files in {}", directory.c_str());
        return false;
    }
    fmt::println("Files are ready for benchmark");
    return true;
}

/** The half-open range of input files one import thread takes. */
struct files_share_t {

    /** First file of the share. */
    std::size_t begin;

    /** One past the last file of the share. */
    std::size_t end;
};

/** Divides @p files_count into one contiguous share per thread of @p state, the last taking the remainder. */
[[nodiscard]] files_share_t files_share(bm::State const& state, std::size_t files_count) noexcept {
    auto const threads_count = static_cast<std::size_t>(state.threads());
    auto const thread_index = static_cast<std::size_t>(state.thread_index());
    std::size_t const share = files_count / threads_count;
    std::size_t const begin = thread_index * share;
    return {begin, thread_index + 1 == threads_count ? files_count : begin + share};
}

/** Seconds elapsed since @p start. */
[[nodiscard]] double seconds_since(std::chrono::steady_clock::time_point start) noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

/** Answers the number of keys in the main collection. */
[[nodiscard]] std::size_t count_keys(ustore_database_t db) noexcept {
    ustore_arena_t arena = nullptr;
    ustore_error_t error {};
    ustore_collection_t const collection = ustore_collection_main_k;
    ustore_key_t const start = std::numeric_limits<ustore_key_t>::min();
    ustore_length_t const limit = std::numeric_limits<ustore_length_t>::max();
    ustore_length_t* counts = nullptr;
    ustore_key_t* keys = nullptr;
    ustore_scan_t scan {};
    scan.db = db;
    scan.error = &error;
    scan.arena = &arena;
    scan.tasks_count = 1;
    scan.collections = &collection;
    scan.start_keys = &start;
    scan.count_limits = &limit;
    scan.counts = &counts;
    scan.keys = &keys;
    ustore_scan(&scan);
    std::size_t const count = error.status ? 0 : counts[0];
    ustore_arena_free(arena);
    return count;
}

/** Drops the keys and values of the main collection, reporting a failure on the standard error. */
void clear_main(ustore_database_t db) noexcept {
    ustore_error_t error {};
    ustore_collection_drop_t drop {};
    drop.db = db;
    drop.error = &error;
    drop.id = ustore_collection_main_k;
    drop.mode = ustore_drop_keys_vals_k;
    ustore_collection_drop(&drop);
    if (error.status) fmt::println(stderr, "Failed to clear the main collection: {}", error.message);
}

/** Answers the size of the exported file at @p path and removes it, or zero when it is missing. */
[[nodiscard]] std::size_t take_exported(char const* path) noexcept {
    struct stat metadata {};
    if (stat(path, &metadata) != 0) return 0;
    [[maybe_unused]] int const removed = unlink(path);
    return static_cast<std::size_t>(metadata.st_size);
}

/** Composes the path of the export named @p stem that the thread at @p thread_index writes, in the input format. */
[[nodiscard]] bool export_path(sz::string_t& path, settings_t const& settings, std::string_view stem,
                               std::size_t thread_index) noexcept {
    std::string_view const extension = format_name(selected_format(settings));
    char infix[24];
    auto const written = fmt::format_to_n(infix, sizeof(infix) - 1, "-{}.", thread_index);
    *written.out = '\0';
    return path.try_append(settings.directory.data(), settings.directory.size()) && path.try_push_back('/') &&
           path.try_append(stem.data(), stem.size()) && path.try_append(infix) &&
           path.try_append(extension.data(), extension.size());
}

void bench_docs_import(bm::State& state, tabular_t const& tabular, settings_t const& settings) {
    ustore_arena_t arena = nullptr;
    ustore_error_t error {};
    std::size_t imported = 0;
    files_share_t const share = files_share(state, tabular.files.size());
    std::size_t file = share.begin;

    auto const start = std::chrono::steady_clock::now();
    for ([[maybe_unused]] auto iteration : state) {
        if (file >= share.end) {
            state.SkipWithError("Ran out of input files");
            break;
        }
        ustore_docs_import_t importing {};
        importing.db = tabular.db;
        importing.error = &error;
        importing.arena = &arena;
        importing.collection = ustore_collection_main_k;
        importing.path = tabular.path(file);
        importing.max_batch_size = settings.memory_limit.value;
        importing.id_field = settings.id_field.data();
        ustore_docs_import(&importing);
        if (!error.status) imported += tabular.files[file].size;
        error = {};
        ++file;
    }
    double const duration = seconds_since(start);
    ustore_arena_free(arena);

    state.counters["bytes/s"] = bm::Counter(imported / duration);
    state.counters["duration"] = bm::Counter(duration, bm::Counter::kAvgThreads);
    state.counters["imported"] = bm::Counter(imported);
}

void bench_graph_import(bm::State& state, tabular_t const& tabular, settings_t const& settings) {
    ustore_arena_t arena = nullptr;
    ustore_error_t error {};
    files_share_t const share = files_share(state, tabular.files.size());
    std::size_t file = share.begin;

    auto const start = std::chrono::steady_clock::now();
    for ([[maybe_unused]] auto iteration : state) {
        if (file >= share.end) {
            state.SkipWithError("Ran out of input files");
            break;
        }
        ustore_graph_import_t importing {};
        importing.db = tabular.db;
        importing.error = &error;
        importing.arena = &arena;
        importing.collection = ustore_collection_main_k;
        importing.path = tabular.path(file);
        importing.max_batch_size = settings.memory_limit.value;
        importing.source_id_field = settings.source_field.data();
        importing.target_id_field = settings.target_field.data();
        importing.edge_id_field = settings.edge_field.data();
        ustore_graph_import(&importing);
        error = {};
        ++file;
    }
    double const duration = seconds_since(start);
    ustore_arena_free(arena);

    std::size_t const imported = count_keys(tabular.db) * 3 * sizeof(ustore_key_t);
    state.counters["bytes/s"] = bm::Counter(imported / duration);
    state.counters["duration"] = bm::Counter(duration, bm::Counter::kAvgThreads);
    state.counters["imported"] = bm::Counter(imported);
}

void bench_docs_export(bm::State& state, tabular_t const& tabular, settings_t const& settings) {
    sz::string_t output;
    if (!export_path(output, settings, "tabular_graph_docs", static_cast<std::size_t>(state.thread_index()))) {
        state.SkipWithError("Failed to allocate the output path");
        return;
    }

    ustore_arena_t arena = nullptr;
    ustore_error_t error {};
    std::size_t exported = 0;
    auto const start = std::chrono::steady_clock::now();
    for ([[maybe_unused]] auto iteration : state) {
        ustore_docs_export_t exporting {};
        exporting.db = tabular.db;
        exporting.error = &error;
        exporting.arena = &arena;
        exporting.collection = ustore_collection_main_k;
        exporting.path = output.c_str();
        exporting.max_batch_size = settings.memory_limit.value;
        ustore_docs_export(&exporting);
        if (!error.status) exported += take_exported(output.c_str());
        error = {};
    }
    double const duration = seconds_since(start);
    ustore_arena_free(arena);

    state.counters["bytes/s"] = bm::Counter(exported / duration);
    state.counters["duration"] = bm::Counter(duration, bm::Counter::kAvgThreads);
    state.counters["exported"] = bm::Counter(exported);
}

void bench_graph_export(bm::State& state, tabular_t const& tabular, settings_t const& settings) {
    sz::string_t output;
    sz::string_t source;
    sz::string_t target;
    sz::string_t edge;
    if (!export_path(output, settings, "tabular_graph_edges", static_cast<std::size_t>(state.thread_index())) ||
        !source.try_append(settings.source_field.data(), settings.source_field.size()) ||
        !target.try_append(settings.target_field.data(), settings.target_field.size()) ||
        !edge.try_append(settings.edge_field.data(), settings.edge_field.size())) {
        state.SkipWithError("Failed to allocate the export names");
        return;
    }
    // Exported column names cannot hold the slashes of JSON pointers.
    for (sz::string_t* name : {&source, &target, &edge})
        for (std::size_t index = 0; index != name->size(); ++index)
            if ((*name)[index] == '/') (*name)[index] = '_';

    ustore_arena_t arena = nullptr;
    ustore_error_t error {};
    std::size_t exported = 0;
    auto const start = std::chrono::steady_clock::now();
    for ([[maybe_unused]] auto iteration : state) {
        ustore_graph_export_t exporting {};
        exporting.db = tabular.db;
        exporting.error = &error;
        exporting.arena = &arena;
        exporting.collection = ustore_collection_main_k;
        exporting.path = output.c_str();
        exporting.max_batch_size = settings.memory_limit.value;
        exporting.source_id_field = source.c_str();
        exporting.target_id_field = target.c_str();
        exporting.edge_id_field = edge.c_str();
        ustore_graph_export(&exporting);
        if (!error.status) exported += take_exported(output.c_str());
        error = {};
    }
    double const duration = seconds_since(start);
    ustore_arena_free(arena);

    state.counters["bytes/s"] = bm::Counter(exported / duration);
    state.counters["duration"] = bm::Counter(duration, bm::Counter::kAvgThreads);
    state.counters["exported"] = bm::Counter(exported);
}

/** Composes a benchmark name from @p prefix and the name of @p format. */
[[nodiscard]] bool benchmark_name(sz::string_t& name, char const* prefix, file_format_t format) noexcept {
    std::string_view const suffix = format_name(format);
    return name.try_append(prefix) && name.try_append(suffix.data(), suffix.size());
}

} // namespace unum::ustore::bench

int main(int argc, char** argv) {
    using namespace unum::ustore;
    using namespace unum::ustore::bench;

    settings_t settings;
    std::array<flag_t, 11> const flags {{
        {"--dataset", "path", "directory of input files, required", &settings.dataset},
        {"--format", "name", "format of the input files and the exports", &settings.format},
        {"--config", "path", "JSON file configuring the database", &settings.config},
        {"--directory", "path", "directory of the exports and of the database without --config, required",
         &settings.directory},
        {"--id-field", "field", "field holding each document's key, required", &settings.id_field},
        {"--source-field", "field", "integer field holding the source vertex, required", &settings.source_field},
        {"--target-field", "field", "integer field holding the target vertex, required", &settings.target_field},
        {"--edge-field", "field", "field holding the edge key, required", &settings.edge_field},
        {"--threads", "count", "threads every benchmark runs on", &settings.threads},
        {"--max-input-files-total", "count", "input files listed from the dataset", &settings.max_input_files_total},
        {"--memory-limit", "size", "bytes an import or export buffers per batch, like 1GiB", &settings.memory_limit},
    }};

    // Google Benchmark exits on `--help` inside its initialization, so these flags print first
    for (int index = 1; index < argc; ++index)
        if (std::string_view {argv[index]} == "--help") {
            print_usage(argv[0], flags);
            break;
        }
    bm::Initialize(&argc, argv);
    arguments_t const parsed = parse_arguments(argc, argv, flags, {});
    if (parsed.status == argument_status_t::help_k) return 0;
    if (parsed.status != argument_status_t::parsed_k) {
        fmt::println(stderr, "{}: {}", parsed.argument, describe(parsed.status));
        return 1;
    }

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

    sz::string_t config;
    bool const configured = settings.config.empty()
                                ? config.try_append(R"({"version": "1.0", "directory": ")") &&
                                      config.try_append(settings.directory.data(), settings.directory.size()) &&
                                      config.try_append(R"("})")
                                : read_file(settings.config.data(), config);
    if (!configured) {
        fmt::println(stderr, "Failed to prepare the database configuration");
        return 1;
    }

    ustore_error_t error {};
    ustore_database_init_t init {};
    init.config = config.c_str();
    init.db = &tabular.db;
    init.error = &error;
    ustore_database_init(&init);
    if (error.status) {
        fmt::println(stderr, "Failed to open the database: {}", error.message);
        return 1;
    }

    sz::string_t docs_import_name;
    sz::string_t docs_export_name;
    sz::string_t graph_import_name;
    sz::string_t graph_export_name;
    if (!benchmark_name(docs_import_name, "docs_import_", selected_format(settings)) ||
        !benchmark_name(docs_export_name, "docs_export_", selected_format(settings)) ||
        !benchmark_name(graph_import_name, "graph_import_", selected_format(settings)) ||
        !benchmark_name(graph_export_name, "graph_export_", selected_format(settings))) {
        ustore_database_free(tabular.db);
        return 1;
    }

    std::size_t const threads = settings.threads.value;
    std::size_t const files_per_thread = settings.max_input_files_total.value / threads;
    auto const clear = [&tabular](bm::State const&) { clear_main(tabular.db); };
    bm::RegisterBenchmark(docs_import_name.c_str(),
                          [&](bm::State& state) { bench_docs_import(state, tabular, settings); })
        ->Threads(threads)
        ->Iterations(files_per_thread);
    bm::RegisterBenchmark(docs_export_name.c_str(),
                          [&](bm::State& state) { bench_docs_export(state, tabular, settings); })
        ->Threads(threads)
        ->Iterations(1)
        ->Teardown(clear);
    bm::RegisterBenchmark(graph_import_name.c_str(),
                          [&](bm::State& state) { bench_graph_import(state, tabular, settings); })
        ->Threads(threads)
        ->Iterations(files_per_thread);
    bm::RegisterBenchmark(graph_export_name.c_str(),
                          [&](bm::State& state) { bench_graph_export(state, tabular, settings); })
        ->Threads(threads)
        ->Iterations(1)
        ->Teardown(clear);

    bm::RunSpecifiedBenchmarks();
    bm::Shutdown();

    ustore_database_free(tabular.db);
    return 0;
}
