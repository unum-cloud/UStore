/**
 *  @file bench/nvme_single_collection_database.cpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Benchmarks insertion, lookup, selection, scan and removal over single-collection
 *      databases of different value layouts.
 *
 *  Every workload the manifest switches on opens one database over a volume of its own under
 *  <tt>--directory</tt>, fills it with <tt>population_count</tt> keys, then samples finds, single and batched
 *  selects and a scan, and finally removes every key. Each value layout takes its own entry:
 *  <tt>12_bytes</tt> and <tt>44_bytes</tt> keep values inside the key slots, <tt>fixed_width</tt> stores
 *  <tt>value_size</tt> bytes per key, and <tt>variable_width</tt> up to <tt>value_max_size</tt>.
 *
 *  Beside the flags of @c bench/harness.hpp, whose @c --seed also seeds the keys and the value
 *  widths, it takes:
 *
 *  @verbatim
 *  Flag           Default    Meaning
 *  --directory    Required   Directory every workload's volume is created in, and removed from after it
 *  --workloads    Required   JSON manifest of the workloads, like bench/nvme_single_collection_database.json
 *  --transport    automatic  How the volumes are reached, like linux-uring, or memory to skip the drives
 *  --volume-size  4GB        Bytes every workload's volume spans
 *  @endverbatim
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstdio>  // `stderr`

#include <array>        // `std::array`
#include <filesystem>   // `std::filesystem::remove_all`
#include <span>         // `std::span`
#include <string>       // `std::string`
#include <string_view>  // `std::string_view`
#include <system_error> // `std::error_code`
#include <utility>      // `std::move`

#include <fmt/format.h>
#include <yyjson.h> // `yyjson_read_file`, `yyjson_obj_get`, `yyjson_get_uint`, `yyjson_get_bool`

#include <forkunion/atomics.hpp>       // `fu::standard_atomic_ref`
#include <forkunion/types.hpp>         // `fu::standard_yield_t`
#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/blobs/nvme_compaction.hpp"                 // `no_device_fold_t`
#include "ustore/blobs/nvme_single_collection_database.hpp" // `basic_single_collection_database`
#include "ustore/blobs/nvme_store.hpp"                      // `nvme_store`, `nvme_store_cursor`
#include "ustore/utilities/folds.hpp"                       // `last_write_fold_t`
#include "ustore/utilities/nvme_mounts.hpp"                 // `open_resources`, `resources_config_t`, `io_transport_t`

#include "harness.hpp"

namespace unum::ustore::bench {

using namespace unum::ustore::blobs;

/** The atomic reference the database templates take: the portable one, as a benchmark picks
 *  no CPU class. */
template <typename value_type_>
using atomic_reference = fu::standard_atomic_ref<value_type_>;

/** What a thread waiting on the volumes does between polls. */
using waiter_t = fu::standard_yield_t;

using stored_key_t = std::uint64_t;
using resources_ptr_t = resources_shared_ptr<atomic_reference, waiter_t>;

/** Keys one batched select asks for. */
constexpr std::size_t batch_keys_k = 256;

#pragma region Settings

/** The names @c --transport accepts, in the order of @c io_transport_t. */
constexpr std::array<std::string_view, 6> storage_transport_names_k {"automatic", "blocking",   "linux-uring",
                                                                     "linux-aio", "linux-spdk", "memory"};

/** Command-line options of the benchmark, each field set by the flag of the same name. */
struct settings_t : shared_settings_t {

    /** Directory every workload's volume is created in. */
    std::string_view directory;

    /** JSON manifest of the workloads. */
    std::string_view workloads;

    /** How the volumes are reached. */
    choice_t transport {storage_transport_names_k};

    /** Bytes every workload's volume spans. */
    bytes_t volume_size {.value = 4ull * 1024ull * 1024ull * 1024ull};
};

/** Whether the manifest switches a workload on. */
enum class toggle_t : bool {
    disabled_k,
    enabled_k,
};

/** One entry of the manifest: the value layout its name picks and the bounds its
 *  database opens with. */
struct workload_t {
    std::string_view name;
    value_layout_t layout = value_layout_t::unknown_k;
    toggle_t toggle = toggle_t::disabled_k;
    std::size_t threads = 1;
    std::size_t population_count = 0;
    stored_value_size_t value_size = 0;
    stored_value_size_t value_max_size = 0;
    std::size_t cache_limit = 0;
    std::size_t memory_runs_max_deltas = 0;
    std::size_t memory_runs_max_bytes = 0;
    std::size_t level_enlarge_factor = 0;
};

/** The manifest entries, one per value layout. */
constexpr std::array<workload_t, 4> manifest_entries_k {{
    {.name = "12_bytes", .layout = value_layout_t::inline_12_bytes_k},
    {.name = "44_bytes", .layout = value_layout_t::inline_44_bytes_k},
    {.name = "fixed_width", .layout = value_layout_t::fixed_width_k},
    {.name = "variable_width", .layout = value_layout_t::variable_width_k},
}};

/** Reads every entry of the manifest at @p path into @p workloads, answering whether
 *  the file parsed. */
bool read_manifest(std::string_view path, std::array<workload_t, 4>& workloads) noexcept {
    std::string const terminated {path};
    yyjson_doc* const document = yyjson_read_file(terminated.c_str(), 0, nullptr, nullptr);
    if (!document) return false;
    yyjson_val* const root = yyjson_doc_get_root(document);
    workloads = manifest_entries_k;
    for (workload_t& workload : workloads) {
        std::string const name {workload.name};
        yyjson_val* const entry = yyjson_obj_get(root, name.c_str());
        if (!entry) continue;
        auto const number = [entry](char const* field) noexcept {
            return yyjson_get_uint(yyjson_obj_get(entry, field));
        };
        workload.toggle = yyjson_get_bool(yyjson_obj_get(entry, "enabled")) ? toggle_t::enabled_k
                                                                            : toggle_t::disabled_k;
        workload.threads = number("threads") ? number("threads") : 1;
        workload.population_count = number("population_count");
        workload.value_size = static_cast<stored_value_size_t>(number("value_size"));
        workload.value_max_size = static_cast<stored_value_size_t>(number("value_max_size"));
        workload.cache_limit = number("cache_limit");
        workload.memory_runs_max_deltas = number("memory_runs_max_deltas");
        workload.memory_runs_max_bytes = number("memory_runs_max_bytes");
        workload.level_enlarge_factor = number("level_enlarge_factor");
    }
    yyjson_doc_free(document);
    return true;
}

#pragma endregion Settings

#pragma region Workloads

/** The bounds @p workload opens its database with, transactions taking the memory runs' own. */
single_collection_database_config_t database_config(workload_t const& workload) noexcept {
    single_collection_database_config_t config;
    config.collection.concurrency_limit = 1;
    config.collection.cache_limit = workload.cache_limit;
    config.collection.memory_runs_max_bytes = workload.memory_runs_max_bytes;
    config.collection.memory_runs_max_deltas = workload.memory_runs_max_deltas;
    config.collection.lsm_tree.level_enlarge_factor = workload.level_enlarge_factor;
    config.collection.lsm_tree.lsm_level_0.sorted_run.value_size = workload.value_size;
    config.collection.lsm_tree.lsm_level_0.sorted_run.value_max_size = workload.value_max_size;
    config.default_transaction.max_bytes = workload.memory_runs_max_bytes;
    config.default_transaction.max_deltas = workload.memory_runs_max_deltas;
    return config;
}

/** Flushes the memory runs of @p database and merges until no level is over its bounds, outside
 *  every timed row. */
template <typename database_type_>
status_t settle(database_type_& database) noexcept {
    if (status_t const flushed = database.flush(); flushed.failed()) return flushed;
    while (database.needs_compaction())
        if (expected<compaction_outcome_t> const merged = database.compact(); !merged) return merged.status();
    return {};
}

/** Runs every row of @p workload over the volume and the channels of @p mount, with the database
 *  of @p layout_. */
template <value_layout_t layout_, typename mount_type_>
status_t run_rows(mount_type_& mount, resources_ptr_t const& resources, settings_t const& settings,
                  workload_t const& workload) noexcept {
    using volume_t = typename mount_type_::volume_t;
    using slot_t = key_slot<stored_key_t, layout_>;
    using store_t = nvme_store<slot_t, volume_t, atomic_reference, waiter_t, last_write_fold_t, no_device_fold_t>;
    using cursor_t =
        nvme_store_cursor<slot_t, volume_t, atomic_reference, waiter_t, last_write_fold_t, no_device_fold_t>;
    using database_t = basic_single_collection_database<stored_key_t, volume_t, atomic_reference, waiter_t, store_t>;

    auto [writing, writing_status] = mount.channels().acquire();
    if (writing_status.failed()) return writing_status;
    auto [merging, merging_status] = mount.channels().acquire();
    if (merging_status.failed()) return merging_status;
    volume_t& volume = mount.volume();
    auto [database, open_status] = database_t::open(volume, writing.channel(), merging.channel(),
                                                    database_config(workload), resources);
    if (open_status.failed()) return open_status;
    auto maybe_executor = executor_t::make(workload.threads, caller_exclusivity_t::caller_inclusive_k);
    if (!maybe_executor) return {status_code_t::out_of_memory_k, "The executor did not start"};
    executor_t& executor = *maybe_executor;

    // Inline and fixed layouts write values of their whole width, and the variable one draws a
    // width per key
    stored_value_size_t const value_max_size = database->value_max_size();
    st::vector<byte_t> pattern;
    if (st::failed(pattern.resize(value_max_size))) return {status_code_t::out_of_memory_k, "No memory for values"};
    for (std::size_t index = 0; index != pattern.size(); ++index)
        pattern[index] = static_cast<byte_t>(static_cast<unsigned char>(index * 131));
    std::size_t const population = workload.population_count;
    auto const name_of = [&](std::string_view verb) { return fmt::format("{}_{}", workload.name, verb); };

    // Every later row reads what this one wrote, so it runs even when the filter leaves
    // its line out
    std::string const insert_name = name_of("insert");
    row_t const inserted = run_pass(executor, insert_name, population, [&](loop_t& loop, std::size_t thread) noexcept {
        splitmix64_t random {stream_key(settings.seed.value, insert_name, thread)};
        for (std::size_t call : loop) {
            std::size_t const width = layout_ == value_layout_t::variable_width_k
                                          ? 1 + static_cast<std::size_t>(random.below(value_max_size))
                                          : value_max_size;
            if (database->insert(call, std::span<byte_t const> {pattern}.first(width)).failed()) loop.fail();
            else loop.count(1, width);
        }
    });
    if (settings.selects(insert_name)) print(inserted);
    if (status_t const settled = settle(*database); settled.failed()) return settled;

    std::string const find_name = name_of("find");
    if (settings.selects(find_name))
        print(run_threads(executor, settings, find_name, [&](loop_t& loop, std::size_t thread) noexcept {
            splitmix64_t random {stream_key(settings.seed.value, find_name, thread)};
            for ([[maybe_unused]] std::size_t call : loop) {
                stored_value_size_t size = 0;
                lookup_result_t const found = database->find(random.below(population), size);
                if (found != lookup_result_t::written_k) loop.fail();
                else loop.count(1, size);
            }
        }));

    std::string const select_name = name_of("select");
    if (settings.selects(select_name))
        print(run_threads(executor, settings, select_name, [&](loop_t& loop, std::size_t thread) noexcept {
            auto [reading, reading_status] = mount.channels().acquire(thread);
            st::vector<byte_t> value;
            if (reading_status.failed() || st::failed(value.resize(value_max_size)))
                return loop.skip("no channel or no memory for a value");
            splitmix64_t random {stream_key(settings.seed.value, select_name, thread)};
            for ([[maybe_unused]] std::size_t call : loop) {
                stored_value_size_t size = 0;
                status_t const completed = database->select(reading.channel(), random.below(population),
                                                            std::span<byte_t> {value}, size);
                if (completed.failed()) loop.fail();
                else loop.count(1, size);
            }
        }));

    std::string const batch_name = name_of(fmt::format("select_batch/{}", batch_keys_k));
    if (settings.selects(batch_name))
        print(run_threads(executor, settings, batch_name, [&](loop_t& loop, std::size_t thread) noexcept {
            auto [reading, reading_status] = mount.channels().acquire(thread);
            std::array<stored_key_t, batch_keys_k> keys {};
            std::array<lookup_result_t, batch_keys_k> results {};
            if (reading_status.failed()) return loop.skip("no channel");
            splitmix64_t random {stream_key(settings.seed.value, batch_name, thread)};
            for ([[maybe_unused]] std::size_t call : loop) {
                for (stored_key_t& key : keys) key = random.below(population);
                std::size_t bytes = 0;
                status_t const completed = database->select(
                    reading.channel(), std::span<stored_key_t const> {keys}, std::span<lookup_result_t> {results},
                    [&bytes](auto, auto, std::span<byte_t const> piece) noexcept { bytes += piece.size(); });
                if (completed.failed()) loop.fail();
                else loop.count(batch_keys_k, bytes);
            }
        }));

    // Each call reads the value under the cursor and steps past it, starting over once
    // the walk ends
    std::string const scan_name = name_of("scan");
    if (settings.selects(scan_name))
        print(run_threads(executor, settings, scan_name, [&](loop_t& loop, std::size_t thread) noexcept {
            auto [reading, reading_status] = mount.channels().acquire(thread);
            st::vector<byte_t> value;
            if (reading_status.failed() || st::failed(value.resize(value_max_size)))
                return loop.skip("no channel or no memory for a value");
            expected<cursor_t> cursor = cursor_t::over(database->records());
            if (!cursor) return loop.skip("the cursor did not open");
            for ([[maybe_unused]] std::size_t call : loop) {
                if (cursor->reached_end()) cursor = cursor_t::over(database->records());
                if (!cursor) return loop.skip("the cursor did not reopen");
                stored_value_size_t size = 0;
                status_t const completed = database->select(reading.channel(), cursor->stored_key(),
                                                            std::span<byte_t> {value}, size);
                if (completed.failed() || cursor->advance().failed()) loop.fail();
                else loop.count(1, size);
            }
        }));

    std::string const remove_name = name_of("remove");
    if (settings.selects(remove_name)) {
        print(run_pass(executor, remove_name, population, [&](loop_t& loop, std::size_t) noexcept {
            for (std::size_t call : loop)
                if (database->remove(call).failed()) loop.fail();
                else loop.count(1);
        }));
        if (status_t const settled = settle(*database); settled.failed()) return settled;
    }
    return {};
}

/** Opens a fresh volume for @p workload under @c --directory and runs its rows, removing
 *  the volume afterwards. */
template <value_layout_t layout_>
status_t run_workload(settings_t const& settings, workload_t const& workload) noexcept {
    std::string const directory = fmt::format("{}/{}", settings.directory, workload.name);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory, error);
    std::string const manifest = fmt::format("{}/bench.ustore.json", directory);

    resources_config_t config;
    config.threads = workload.threads;
    config.shared_io_channels = 3;
    config.io_transport = static_cast<io_transport_t>(settings.transport.selected);
    data_directory_config_t data;
    data.max_size = settings.volume_size.value;
    if (sz::failed(config.manifest.assign(string_view_t {manifest})) ||
        sz::failed(data.path.assign(string_view_t {directory})) ||
        st::failed(config.data_directories.push_back(std::move(data))))
        return {status_code_t::out_of_memory_k, "No memory for the directories"};

    status_t ran;
    {
        expected<resources_ptr_t> opened = open_resources<atomic_reference, waiter_t>(config);
        if (!opened) return opened.status();
        resources_ptr_t const resources = std::move(*opened);
        fmt::println("- {}: {} transport, {} threads, {} keys", workload.name,
                     storage_transport_names_k[static_cast<std::size_t>(resources->storage.report().transport)],
                     workload.threads, workload.population_count);
        ran = resources->storage.visit(
            [&](auto& mount) noexcept { return run_rows<layout_>(mount, resources, settings, workload); });
    }
    std::filesystem::remove_all(directory, error);
    return ran;
}

#pragma endregion Workloads

} // namespace unum::ustore::bench

int main(int argc, char** argv) {
    using namespace unum::ustore;
    using namespace unum::ustore::bench;

    settings_t settings;
    std::array<flag_t, 4> const flags {{
        {"--directory", "path", "directory every workload's volume is created in", &settings.directory},
        {"--workloads", "path", "JSON manifest of the workloads", &settings.workloads},
        {"--transport", "name", "how the volumes are reached, or memory to skip the drives", &settings.transport},
        {"--volume-size", "bytes", "bytes every workload's volume spans, like 4GB", &settings.volume_size},
    }};
    read_settings(argc, argv, settings, flags);
    if (settings.directory.empty() || settings.workloads.empty()) {
        fmt::println(stderr, "--directory and --workloads are required");
        return 1;
    }
    std::array<workload_t, 4> workloads;
    if (!read_manifest(settings.workloads, workloads)) {
        fmt::println(stderr, "Failed to parse {}", settings.workloads);
        return 1;
    }

    int exit_code = 0;
    for (workload_t const& workload : workloads) {
        if (workload.toggle == toggle_t::disabled_k) continue;
        status_t ran;
        switch (workload.layout) {
        case value_layout_t::inline_12_bytes_k:
            ran = run_workload<value_layout_t::inline_12_bytes_k>(settings, workload);
            break;
        case value_layout_t::inline_44_bytes_k:
            ran = run_workload<value_layout_t::inline_44_bytes_k>(settings, workload);
            break;
        case value_layout_t::fixed_width_k:
            ran = run_workload<value_layout_t::fixed_width_k>(settings, workload);
            break;
        case value_layout_t::variable_width_k:
            ran = run_workload<value_layout_t::variable_width_k>(settings, workload);
            break;
        default: break;
        }
        if (ran.failed()) {
            fmt::println(stderr, "{}: {}", workload.name, ran.message);
            exit_code = 1;
        }
    }
    return exit_code;
}
