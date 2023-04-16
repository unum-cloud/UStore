/**
 * @file tabular_graph.cpp
 * @brief Imports a big Parquet/CSV/NDJSON dataset as a labeled graph.
 * @version 0.1
 * @date 2022-10-02
 *
 * Every row is treated as a separate edge.
 * All of its columns are treated as different document fields, except for
 * - Integer column for source node ID.
 * - Integer column for target node ID.
 * - Optional integer column for document/edge ID.
 * If the last one isn't provided, the row number is used as the document ID.
 *
 * https://arrow.apache.org/docs/cpp/dataset.html#dataset-discovery
 * https://arrow.apache.org/docs/cpp/parquet.html
 * https://arrow.apache.org/docs/cpp/csv.html
 */

#include <cstring>     // `std::memchr`
#include <algorithm>   // `std::search`
#include <filesystem>  // Listing directories is too much pain in C
#include <string_view> //
#include <vector>      //
#include <thread>      //
#include <mutex>       //

#include <benchmark/benchmark.h>
#include <fmt/format.h>
#include <argparse/argparse.hpp>

#include <ustore/ustore.hpp>
#include <dataset.h>

namespace bm = benchmark;
using namespace unum::ustore;

constexpr size_t max_batch_size_k = 1024 * 1024 * 1024;
constexpr ustore_str_view_t path_k = "./";

static database_t db;

static std::vector<size_t> source_sizes;
static std::vector<std::string> source_files;

struct args_t {
    std::string path;
    std::string extension;

    std::string config_path;

    std::string id;

    size_t threads_count;
    size_t files_count;
};

void parse_args(int argc, char* argv[], args_t& args) {
    argparse::ArgumentParser program(argv[0]);
    program.add_argument("-p", "--path").required().help("File path for importing");
    program.add_argument("-e", "--ext").required().help("File extension for exporting");
    program.add_argument("-c", "--cfg").default_value(std::string("")).help("Config path");
    program.add_argument("-i", "--id").required().help("Id field");
    program.add_argument("-th", "--threads").default_value(std::string("1")).help("Threads count");
    program.add_argument("-m", "--max_input_files").default_value(std::string("10")).help("Max input files count");

    program.parse_known_args(argc, argv);

    args.path = program.get("path");
    args.extension = program.get("ext");
    args.config_path = program.get("cfg");
    args.id = program.get("id");
    args.threads_count = std::stoi(program.get("threads"));
    args.files_count = std::stoi(program.get("max_input_files"));

    if (args.threads_count == 0) {
        fmt::print("Zero threads count specified\n");
        exit(1);
    }

    if (args.files_count == 0) {
        fmt::print("Zero max input files count specified\n");
        exit(1);
    }
}

static void bench_docs_import(bm::State& state, args_t const& args) {
    auto collection = db.main();
    status_t status;
    arena_t arena(db);

    size_t size = 0;
    size_t idx = 0;
    size_t pos = state.thread_index() * args.files_count;

    auto start = std::chrono::high_resolution_clock::now();
    for (auto _ : state) {
        ustore_docs_import_t docs {};
        docs.db = db;
        docs.error = status.member_ptr();
        docs.arena = arena.member_ptr();
        docs.collection = collection;
        docs.paths_pattern = source_files[pos + idx].c_str();
        docs.max_batch_size = max_batch_size_k;
        docs.id_field = args.id.c_str();
        ustore_docs_import(&docs);

        if (status)
            size += source_sizes[pos + idx];
        else
            status.release_error();
        ++idx;
    }
    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(end - start).count() / 1000;
    state.counters["bytes/s"] = bm::Counter(size / duration);
    state.counters["duration"] = bm::Counter(duration, bm::Counter::kAvgThreads);
    state.counters["imported"] = bm::Counter(size);
}

size_t find_and_delete() {
    static std::mutex mtx;
    mtx.lock();
    time_t now = time(0);
    std::string exp = ctime(&now);
    exp = exp.substr(0, 10);
    for (auto& ch : exp) {
        if ((ch == ' ') | (ch == ':'))
            ch = '_';
    }
    for (const auto& entry : std::filesystem::directory_iterator(path_k)) {
        if (entry.path().string().substr(2, 10) == exp) {
            size_t size = entry.file_size();
            std::remove(entry.path().c_str());
            mtx.unlock();
            return size;
        }
    }
    mtx.unlock();
    return 0;
}

static void bench_docs_export(bm::State& state, args_t const& args) {
    auto collection = db.main();
    arena_t arena(db);
    status_t status;

    size_t size = 0;

    auto start = std::chrono::high_resolution_clock::now();
    for (auto _ : state) {
        ustore_docs_export_t docs {};
        docs.db = db;
        docs.error = status.member_ptr();
        docs.arena = arena.member_ptr();
        docs.collection = collection;
        docs.paths_extension = args.extension.c_str();
        docs.max_batch_size = max_batch_size_k;
        ustore_docs_export(&docs);
        if (status)
            size += find_and_delete();
        else
            status.release_error();
    }
    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(end - start).count() / 1000;
    state.counters["bytes/s"] = bm::Counter(size / duration);
    state.counters["duration"] = bm::Counter(duration);
    state.counters["exported"] = bm::Counter(size);
    db.clear().throw_unhandled();
}

void parse_paths(args_t& args) {
    fmt::print("Will search for {} files...\n", args.extension);
    auto dataset_path = args.path;
    auto home_path = std::getenv("HOME");
    if (dataset_path.front() == '~')
        dataset_path = std::filesystem::path(home_path) / dataset_path.substr(2);
    auto opts = std::filesystem::directory_options::follow_directory_symlink;
    for (auto const& dir_entry : std::filesystem::directory_iterator(dataset_path, opts)) {
        if (dir_entry.path().extension() != args.extension)
            continue;

        source_files.push_back(dir_entry.path());
        source_sizes.push_back(dir_entry.file_size());
    }
    size_t files_count = std::min(source_files.size(), args.files_count * args.threads_count);
    source_files.resize(files_count);
    source_sizes.resize(files_count);
    fmt::print("Files are ready for benchmark\n");
}

void bench_docs(args_t const& args) {
    bm::RegisterBenchmark(fmt::format("docs_import_{}", args.extension.substr(1)).c_str(),
                          [&](bm::State& s) { bench_docs_import(s, args); })
        ->Threads(args.threads_count)
        ->Iterations(args.files_count);
    bm::RegisterBenchmark(fmt::format("docs_export_{}", args.extension.substr(1)).c_str(),
                          [&](bm::State& s) { bench_docs_export(s, args); });
}

int main(int argc, char** argv) {

    args_t args {};
    parse_args(argc, argv, args);
    parse_paths(args);
    bm::Initialize(&argc, argv);
    db.open(args.config_path.c_str()).throw_unhandled();

    bench_docs(args);

    bm::RunSpecifiedBenchmarks();
    bm::Shutdown();

    db.close();

    return 0;
}