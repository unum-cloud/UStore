/**
 *  @file tools/nvme_importer.cu
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Imports files from a directory tree into a UStoreNVMe database collection, keyed by inode number.
 */

#include <cstdint>
#include <cstdio>

#include <array>
#include <atomic>
#include <filesystem>
#include <limits>
#include <string_view>

#include <fmt/format.h>

#include "arguments.hpp"
#include "ustore/blobs/nvme_database.hpp"  // `database`
#include "ustore/blobs/nvme_tree.hpp"      // `user_collection_config_t`
#include "ustore/utilities/ram_device.hpp" // `executor_t`

using namespace unum::ustore;
using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;
using namespace unum;

using stored_key_t = std::uint64_t;
using storage_t = database<stored_key_t>;
using storage_config_t = database_config_t;
using collection_id_t = collection_numeric_id_t;

/** Flags of the importer, with their defaults; no path has one. */
struct options_t {

    /** Directory tree the files are read from. */
    std::string_view input_directory;

    /** Suffix every imported file name ends with. */
    std::string_view extension = "jpg";

    /** Largest file imported; larger ones are skipped. */
    bytes_t max_file_size {100 << 10};

    /** Whether subdirectories are scanned too. */
    switch_t recursive = switch_t::absent_k;

    /** Threads inserting files, besides the one printing progress. */
    count_t threads {16};

    /** Database the collection lives in, which also names the log file. */
    std::string_view database_name = "wiki";

    /** Collection the files are inserted into. */
    std::string_view collection = "images";

    /** Metadata directory, erased before the import. */
    std::string_view directory;

    /** Data directories, each erased before the import. */
    texts_t data_directories;

    /** Bytes each data directory may take. */
    bytes_t data_directory_capacity {std::size_t {1} << 40};
};

/** The metadata directory, the log file name and the capped data directories @p options names. */
resources_config_t build_resources_config(options_t const& options) noexcept {

    resources_config_t config;

    config.directory = string_t(options.directory.data(), options.directory.size());

    config.data_directories.clear();
    for (std::string_view const path : options.data_directories.view())
        config.data_directories.push_back({string_t(path.data(), path.size()), options.data_directory_capacity.value});

    return config;
}

/** The write buffer, level and value limits @p options names. */
user_collection_config_t build_collection_config(options_t const& options) noexcept {

    user_collection_config_t config;
    config.concurrency_limit = options.threads.value;
    config.write_buffer_max_elements = 100'000;
    config.write_buffer_max_bytes = 200 * 1024 * 1024ull;
    config.value_size = 0;
    config.value_max_size = options.max_file_size.value;

    return config;
}

/** The files under @p directory_path ending in @p extension and no larger than @p file_max_size. */
darray_gt<string_t> list_files(string_t const& directory_path, switch_t recursive, std::string_view extension,
                               size_t file_max_size) {

    darray_gt<string_t> paths;
    auto iterate = [&](auto&& entries) {
        for (auto const& entry : entries) {
            std::string_view const entry_path = entry.path().native();
            if (entry_path.ends_with(extension) && std::filesystem::file_size(entry.path()) <= file_max_size)
                paths.push_back(entry_path.data());
        }
    };
    if (recursive == switch_t::present_k)
        iterate(std::filesystem::recursive_directory_iterator(directory_path.c_str()));
    else iterate(std::filesystem::directory_iterator(directory_path.c_str()));

    return paths;
}

/** Reads the file at @p path into @p buffer, writing its inode into @p id, and answers the bytes read. */
size_t read_file(string_t const& path, std::uint64_t& id, dbuffer_t& buffer) {

    countdown_t countdown;
    auto file = open_file(path, O_RDONLY, S_IRWXU, countdown);
    if (countdown.has_failed()) return 0;

    size_t read_count = 0;
    auto descriptor = static_cast<int>(file);
    struct stat file_status;
    if (fstat(descriptor, &file_status) == 0) {
        id = file_status.st_ino;
        if (buffer.size() < size_t(file_status.st_size)) buffer = dbuffer_t(file_status.st_size);
        countdown.reset();
        read_from_file(file, buffer.span(), countdown);
        if (countdown.wait()) read_count = file_status.st_size;
    }

    countdown.reset();
    close_file(file, countdown);

    return read_count;
}

/** Inserts every file of @p file_paths into @p collection_id on @p threads_count threads, printing progress. */
template <typename storage_>
void import_files_into_storage(spanc_gt<string_t> file_paths, storage_& storage, collection_id_t collection_id,
                               size_t threads_count) {

    std::atomic_size_t processed_files_count = 0;

    auto inserter = [&](spanc_gt<string_t> files) noexcept {
        dbuffer_t buffer;
        for (size_t index = 0; index < files.size(); ++index) {
            stored_key_t key = std::numeric_limits<stored_key_t>::max();
            auto read = read_file(files[index], key, buffer);
            if (read) {
                auto status = storage.insert(collection_id, key, buffer.view().first_n(read));
                if (!status) fmt::println("Error: Failed to insert: {}", status_to_message(status));
            }
            ++processed_files_count;
        }
    };

    auto print_progress = [&]() noexcept {
        bool first_time = true;
        size_t last_printed_count = 0;
        while (processed_files_count != file_paths.size()) {
            bool print = processed_files_count - last_printed_count > file_paths.size() / 20 ||
                         processed_files_count - last_printed_count > 1'000;
            if (first_time || print) {
                auto done_percent = 100.0 * processed_files_count / file_paths.size();
                fmt::print("\rImported: {:6.2f}%", done_percent);
                fflush(stdout);
                last_printed_count = processed_files_count;
                first_time = false;
            }
        }
    };

    // One extra thread prints progress while the others insert their slices
    auto executor = executor_t::make(threads_count + 1, caller_exclusivity_t::caller_inclusive_k);
    if (!executor) {
        fmt::println("Error: Failed to spawn {} threads", threads_count + 1);
        return;
    }

    size_t files_per_thread = file_paths.size() / threads_count;
    auto const dispatched = executor->for_threads([&](size_t thread) noexcept {
        if (thread == threads_count) return print_progress();
        size_t offset = thread * files_per_thread;
        inserter(
            file_paths.subspan(offset, thread < threads_count - 1 ? files_per_thread : file_paths.size() - offset));
    });
    if (failed(dispatched)) fmt::println("Error: Failed to dispatch the importer threads");
}

int main(int argc, char** argv) {

    options_t options;
    std::array<flag_t, 10> const flags {{
        {"--input-directory", "path", "directory tree the files are read from", &options.input_directory},
        {"--extension", "suffix", "suffix every imported file name ends with", &options.extension},
        {"--max-file-size", "size", "largest file imported; larger ones are skipped", &options.max_file_size},
        {"--recursive", "", "scan subdirectories too", &options.recursive},
        {"--threads", "count", "threads inserting files", &options.threads},
        {"--database-name", "name", "database the collection lives in", &options.database_name},
        {"--collection", "name", "collection the files are inserted into", &options.collection},
        {"--directory", "path", "metadata directory, erased before the import", &options.directory},
        {"--data-directories", "paths", "comma-separated data directories, erased before the import",
         &options.data_directories},
        {"--data-directory-capacity", "size", "bytes each data directory may take", &options.data_directory_capacity},
    }};
    arguments_t const parsed = parse_arguments(argc, argv, flags, {});
    if (parsed.status == argument_status_t::help_k) {
        print_usage("nvme_importer --input-directory <path> --directory <path> [flags]", flags);
        return 0;
    }
    if (parsed.status != argument_status_t::parsed_k) {
        fmt::println(stderr, "{}: {}", parsed.argument, describe(parsed.status));
        return 1;
    }
    if (options.input_directory.empty() || options.directory.empty()) {
        fmt::println(stderr, "--input-directory and --directory are required");
        return 1;
    }
    if (options.threads.value == 0) {
        fmt::println(stderr, "--threads: must be positive");
        return 1;
    }

    auto resources_config = build_resources_config(options);
    storage_config_t storage_config;
    auto collection_config = build_collection_config(options);

    std::filesystem::remove_all(resources_config.directory.c_str());
    for (auto const& storage_disk : resources_config.data_directories)
        std::filesystem::remove_all(storage_disk.path.c_str());

    countdown_t countdown;
    make_path(resources_config.directory, countdown);
    if (countdown.has_failed()) {
        auto message = countdown.release_exception().message();
        fmt::println("Error: Failed to create '{}' dir: {}", resources_config.directory.c_str(), message);
        return 1;
    }
    for (auto const& storage_disk : resources_config.data_directories) {
        countdown.reset();
        make_path(storage_disk.path, countdown);
        if (countdown.has_failed()) {
            auto message = countdown.release_exception().message();
            fmt::println("Error: Failed to create '{}' dir: {}", resources_config.directory.c_str(), message);
            return 1;
        }
    }

    auto print_issue = [](issue_kind_t kind, std::string_view message) noexcept {
        fmt::println("{}: issue: {}", kind == issue_kind_t::error_k ? "Error" : "Warning", message);
    };
    if (!validate(resources_config, storage_config, print_issue)) return 1;

    auto [resources, open_resources_status] = open_resources(resources_config);
    if (!open_resources_status) {
        fmt::println("Error: Failed to open resources: {}", status_to_message(open_resources_status));
        return 1;
    }

    string_t const storage_name(options.database_name.data(), options.database_name.size());
    auto [storage, open_storage_status] = storage_t::open(storage_name, resources_config.directory, storage_config,
                                                          resources);
    if (!open_storage_status) {
        close_resources(resources);
        fmt::println("Error: Failed to open storage: {}", status_to_message(open_storage_status));
        return 1;
    }

    string_t const collection_name(options.collection.data(), options.collection.size());
    auto [collection_id, create_collection_status] = storage->create_collection(
        collection_name, create_collection_config(collection_config));
    if (!create_collection_status) {
        storage.reset();
        close_resources(resources);
        fmt::println("Error: Failed to create collection: {}", status_to_message(create_collection_status));
        return 1;
    }

    string_t const input_directory(options.input_directory.data(), options.input_directory.size());
    darray_gt<string_t> file_paths = list_files(input_directory, options.recursive, options.extension,
                                                options.max_file_size.value);
    import_files_into_storage(file_paths.view(), *storage, collection_id, options.threads.value);

    storage.reset();
    close_resources(resources);

    return 0;
}
