/**
 *  @file bench/nvme_single_collection_database.cu
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Benchmarks insertion, lookup, selection, scan and deletion over single-collection databases of different
 *      value sizes.
 */

#include <cstdio>

#include <array>
#include <string_view>

#include <fmt/format.h>
#include <utils/benchmark.hpp>
#include <utils/system_call.hpp>

#include "arguments.hpp"

#include "ustore/blobs/nvme_single_collection_database.hpp"

using namespace unum::io;
using namespace unum::ustore;
using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

using stored_key_t = std::size_t;

/** Flags of this benchmark. */
struct options_t {

    /** Directory the single-collection databases and the log are written in. */
    std::string_view directory;
};

resources_shared_ptr_t resources;

rand::uniform_gt<stored_key_t> random_stored_key;
rand::uniform_gt<stored_value_size_t> random_size;

struct single_collection_database_initial_config_t {

    bool enabled = false;
    std::size_t threads_count = 0;

    string_t name;
    std::size_t population_count = 0;

    stored_value_size_t value_size = 0;
    stored_value_size_t value_max_size = 0;
    std::size_t cache_limit = 0;
    std::size_t memory_runs_max_bytes = 0;
    std::size_t memory_runs_max_deltas = 0;
    std::size_t level_enlarge_factor = 0;
};

struct bench_config_t {
    bench_config_t() {
        single_collection_database_12_bytes.name = "Italy";
        single_collection_database_44_bytes.name = "US";
        single_collection_database_fixed_width.name = "Germany";
        single_collection_database_variable_width.name = "Russia";
    }

    single_collection_database_initial_config_t single_collection_database_12_bytes;
    single_collection_database_initial_config_t single_collection_database_44_bytes;
    single_collection_database_initial_config_t single_collection_database_fixed_width;
    single_collection_database_initial_config_t single_collection_database_variable_width;
    string_t folder;
};

template <value_layout_t value_layout_>
std::shared_ptr<single_collection_database<stored_key_t, value_layout_>> open_single_collection_database(
    string_t const& folder, single_collection_database_initial_config_t const& initial_config,
    resources_shared_ptr_t resources) {

    using single_collection_database_t = single_collection_database<stored_key_t, value_layout_>;

    single_collection_database_config_t config;

    config.default_transaction.max_bytes = initial_config.memory_runs_max_bytes;
    config.default_transaction.max_deltas = initial_config.memory_runs_max_deltas;

    config.collection.concurrency_limit = 1;
    config.collection.cache_limit = initial_config.cache_limit;
    config.collection.memory_runs_max_bytes = initial_config.memory_runs_max_bytes;
    config.collection.memory_runs_max_deltas = initial_config.memory_runs_max_deltas;

    config.collection.lsm_tree.level_enlarge_factor = initial_config.level_enlarge_factor;

    config.collection.lsm_tree.lsm_level_0.sorted_run.value_size = initial_config.value_size;
    config.collection.lsm_tree.lsm_level_0.sorted_run.value_max_size = initial_config.value_max_size;

    return single_collection_database_t::open(initial_config.name, folder, config, resources).take_value();
}

struct single_collection_databases_t {

    using single_collection_database_12_bytes_t =
        single_collection_database<stored_key_t, value_layout_t::inline_12_bytes_k>;
    using single_collection_database_44_bytes_t =
        single_collection_database<stored_key_t, value_layout_t::inline_44_bytes_k>;
    using single_collection_database_fixed_width_t =
        single_collection_database<stored_key_t, value_layout_t::fixed_width_k>;
    using single_collection_database_variable_width_t =
        single_collection_database<stored_key_t, value_layout_t::variable_width_k>;

    inline single_collection_databases_t(bench_config_t const& config, resources_shared_ptr_t resources)
        : config(config), resources(resources), single_collection_database_12_bytes(nullptr),
          single_collection_database_44_bytes(nullptr), single_collection_database_fixed_width(nullptr),
          single_collection_database_variable_width(nullptr) {}

    void open() {
        if (config.single_collection_database_12_bytes.enabled)
            single_collection_database_12_bytes = open_single_collection_database<value_layout_t::inline_12_bytes_k>(
                config.folder, config.single_collection_database_12_bytes, resources);
        if (config.single_collection_database_44_bytes.enabled)
            single_collection_database_44_bytes = open_single_collection_database<value_layout_t::inline_44_bytes_k>(
                config.folder, config.single_collection_database_44_bytes, resources);
        if (config.single_collection_database_fixed_width.enabled)
            single_collection_database_fixed_width = open_single_collection_database<value_layout_t::fixed_width_k>(
                config.folder, config.single_collection_database_fixed_width, resources);
        if (config.single_collection_database_variable_width.enabled)
            single_collection_database_variable_width =
                open_single_collection_database<value_layout_t::variable_width_k>(
                    config.folder, config.single_collection_database_variable_width, resources);
    }

    void close() {
        single_collection_database_12_bytes.reset();
        single_collection_database_44_bytes.reset();
        single_collection_database_fixed_width.reset();
        single_collection_database_variable_width.reset();
    }

    void destroy() {
        single_collection_database_12_bytes->destroy();
        single_collection_database_44_bytes->destroy();
        single_collection_database_fixed_width->destroy();
        single_collection_database_variable_width->destroy();
    }

    bench_config_t config;
    resources_shared_ptr_t resources;

    std::shared_ptr<single_collection_database_12_bytes_t> single_collection_database_12_bytes;
    std::shared_ptr<single_collection_database_44_bytes_t> single_collection_database_44_bytes;
    std::shared_ptr<single_collection_database_fixed_width_t> single_collection_database_fixed_width;
    std::shared_ptr<single_collection_database_variable_width_t> single_collection_database_variable_width;
};

void initialize_posix_disk_router(string_t const& folder) {

    if (resources->fibers) resources->fibers->stop_and_shutdown();
    resources->disk_router.reset();

    countdown_t countdown;
    resources->fibers = std::make_shared<pool::fibers_t>(9, 1);
    disk_config_t disk_config {folder, unlimited_resource_k};
    resources->disk_router = create_posix_disk_router(only(disk_config), resources->fibers, countdown);
    countdown.throw_unhandled(*resources->logger);
}

void initialize_pulling_disk_router(string_t const& folder) {

    if (resources->fibers) resources->fibers->stop_and_shutdown();
    resources->disk_router.reset();

    countdown_t countdown;
    resources->fibers = std::make_shared<pool::fibers_t>(9, 1);
    disk_config_t disk_config {folder, unlimited_resource_k};
    resources->disk_router = create_pulling_disk_router(only(disk_config), resources->fibers, 256, countdown);
    countdown.throw_unhandled(*resources->logger);
}

void create_stored_value(stored_value_size_t value_size, stored_value_size_t value_max_size, buffer_t& stored_value) {

    if (value_size != 0) stored_value = buffer_t(value_size);
    else {
        auto size = 1 + random_size.until_exclusive(value_max_size);
        stored_value = buffer_t(size);
    }
}

template <value_layout_t value_layout_>
void insert(bm::State& state, single_collection_database_initial_config_t const& initial_config,
            single_collection_database<stored_key_t, value_layout_>& single_collection_database) {

    static std::size_t operations_count = 0;
    static std::size_t bytes_processed_count = 0;
    static std::size_t fails = 0;

    if (state.thread_index() == 0) {
        if (!geteuid()) drop_system_caches();
        operations_count = 0;
        bytes_processed_count = 0;
    }

    while (state.KeepRunningBatch(initial_config.population_count)) {
        std::size_t iterations = initial_config.population_count;
        while (iterations) {
            state.PauseTiming();
            auto stored_key = atomic_increment_fetch(operations_count);
            if ((operations_count % initial_config.memory_runs_max_deltas) == initial_config.memory_runs_max_deltas - 1)
                --stored_key;
            buffer_t stored_value;
            create_stored_value(initial_config.value_size, initial_config.value_max_size, stored_value);
            atomic_add_fetch(bytes_processed_count, std::size_t(stored_value.size()));
            state.ResumeTiming();

            auto status = single_collection_database.insert(stored_key, stored_value.view());

            atomic_add_fetch(fails, std::size_t(status.failed()));
            atomic_add_fetch(bytes_processed_count, std::size_t(status) * stored_value.size());

            --iterations;
        }
    }

    if (state.thread_index() == 0) {
        state.SetBytesProcessed(bytes_processed_count);
        state.counters["inserts/s"] = bm::Counter(operations_count - fails, bm::Counter::kIsRate);
        state.counters["bytes_written"] = bm::Counter(bytes_processed_count, bm::Counter::kDefaults,
                                                      bm::Counter::kIs1024);
        state.counters["fails,%"] = bm::Counter(fails * 100.0 / operations_count);
    }

    single_collection_database.flush();
    single_collection_database.wait_for_pending_background_compactions();
}

template <value_layout_t value_layout_>
void find_only(bm::State& state, single_collection_database_initial_config_t const& initial_config,
               single_collection_database<stored_key_t, value_layout_>& single_collection_database) {

    static std::size_t operations_count = 0;
    static std::size_t fails = 0;

    if (state.thread_index() == 0) {
        if (!geteuid()) drop_system_caches();
        operations_count = 0;
        fails = 0;
    }

    for (auto _ : state) {
        auto stored_key = random_stored_key.until_exclusive(initial_config.population_count);
        auto ok = single_collection_database.contains(stored_key);
        atomic_increment_fetch(operations_count);
        atomic_add_fetch(fails, std::size_t(bool(!ok)));
    }

    if (state.thread_index() == 0) {
        state.counters["finds/s"] = bm::Counter(operations_count, bm::Counter::kIsRate);
        state.counters["fails,%"] = bm::Counter(fails * 100.0 / operations_count);
    }
}

template <value_layout_t value_layout_>
void select(bm::State& state, single_collection_database_initial_config_t const& initial_config,
            single_collection_database<stored_key_t, value_layout_>& single_collection_database) {

    static std::size_t operations_count = 0;
    static std::size_t fails = 0;
    static std::size_t bytes_processed_count = 0;

    if (state.thread_index() == 0) {
        if (!geteuid()) drop_system_caches();
        operations_count = 0;
        fails = 0;
        bytes_processed_count = 0;
    }

    aligned_buffer_t buffer(single_collection_database.buffer_size_for_any_stored_value());

    for (auto _ : state) {
        auto stored_key = random_stored_key.until_exclusive(initial_config.population_count);
        stored_value_size_t size = 0;
        auto status = single_collection_database.select(stored_key, buffer.span(), size);
        if (status.succeeded()) {
            atomic_increment_fetch(operations_count);
            atomic_add_fetch(bytes_processed_count, std::size_t(size));
        }
        else atomic_add_fetch(fails, std::size_t(1));
    }

    if (state.thread_index() == 0) {
        state.SetBytesProcessed(bytes_processed_count);
        state.counters["selects/s"] = bm::Counter(operations_count, bm::Counter::kIsRate);
        state.counters["bytes_read"] = bm::Counter(bytes_processed_count, bm::Counter::kDefaults, bm::Counter::kIs1024);
        state.counters["fails,%"] = bm::Counter(fails * 100.0 / operations_count);
    }
}

template <value_layout_t value_layout_>
void batch_select(bm::State& state, single_collection_database_initial_config_t const& initial_config,
                  single_collection_database<stored_key_t, value_layout_>& benchmarked_single_collection_database,
                  std::size_t queue_depth) {

    using stored_value_locations_t =
        typename single_collection_database<stored_key_t, value_layout_>::stored_value_locations<>;

    static std::size_t operations_count = 0;
    static std::size_t fails = 0;
    static std::size_t bytes_processed_count = 0;

    if (state.thread_index() == 0) {
        if (!geteuid()) drop_system_caches();
        operations_count = 0;
        fails = 0;
        bytes_processed_count = 0;
    }

    aligned_buffer_t buffer(benchmarked_single_collection_database.buffer_size_for_any_stored_value());
    st::basic_vector<stored_key_t> stored_keys(queue_depth);
    stored_value_locations_t stored_value_locations(queue_depth);

    for (auto _ : state) {
        state.PauseTiming();
        for (std::size_t index = 0; index < queue_depth; ++index)
            stored_keys[index] = random_stored_key.until_exclusive(initial_config.population_count);
        state.ResumeTiming();

        benchmarked_single_collection_database.find(stored_keys.view(), stored_value_locations.span());
        countdown_t countdown(queue_depth);
        benchmarked_single_collection_database.select(stored_value_locations.view(), buffer.span(), countdown);
        auto ok = countdown.wait(*resources->fibers);

        atomic_add_fetch(operations_count, std::size_t(ok) * queue_depth);
        atomic_add_fetch(fails, std::size_t(!ok) * queue_depth);
        atomic_add_fetch(bytes_processed_count, std::size_t(ok) * stored_value_locations.total_size_bytes());
    }

    if (state.thread_index() == 0) {
        state.SetBytesProcessed(bytes_processed_count);
        state.counters["batch_selects/s"] = bm::Counter(operations_count, bm::Counter::kIsRate);
        state.counters["bytes_read"] = bm::Counter(bytes_processed_count, bm::Counter::kDefaults, bm::Counter::kIs1024);
        state.counters["fails,%"] = bm::Counter(fails * 100.0 / operations_count);
    }
}

template <value_layout_t value_layout_>
void scan(bm::State& state, single_collection_database<stored_key_t, value_layout_>& single_collection_database) {

    static std::size_t operations_count = 0;
    static std::size_t fails = 0;
    static std::size_t bytes_processed_count = 0;

    if (state.thread_index() == 0) {
        if (!geteuid()) drop_system_caches();
        operations_count = 0;
        fails = 0;
        bytes_processed_count = 0;
    }

    aligned_buffer_t buffer(single_collection_database.buffer_size_for_any_stored_value());

    for (auto _ : state) {
        for (auto iterator = single_collection_database.begin(); iterator != single_collection_database.end();
             ++iterator) {
            countdown_t countdown;
            iterator.get(buffer.span(), countdown);
            atomic_increment_fetch(operations_count);
            atomic_add_fetch(fails, std::size_t(!countdown.wait(*resources->fibers)));
            atomic_add_fetch(bytes_processed_count, std::size_t(iterator.size()));
        }
    }

    if (state.thread_index() == 0) {
        state.SetBytesProcessed(bytes_processed_count);
        state.counters["selects/s"] = bm::Counter(operations_count, bm::Counter::kIsRate);
        state.counters["bytes_read"] = bm::Counter(bytes_processed_count, bm::Counter::kDefaults, bm::Counter::kIs1024);
        state.counters["fails,%"] = bm::Counter(fails * 100.0 / operations_count);
    }
}

template <value_layout_t value_layout_>
void remove(bm::State& state, single_collection_database_initial_config_t const&,
            single_collection_database<stored_key_t, value_layout_>& single_collection_database) {

    static std::size_t operations_count = 0;
    if (state.thread_index() == 0) {
        if (!geteuid()) drop_system_caches();
        operations_count = 0;
    }

    for (auto _ : state) {
        auto stored_key = atomic_increment_fetch(operations_count);
        single_collection_database.remove(stored_key);

        state.PauseTiming();
        atomic_increment_fetch(operations_count);
        state.ResumeTiming();
    }

    if (state.thread_index() == 0) state.counters["removes/s"] = bm::Counter(operations_count, bm::Counter::kIsRate);

    single_collection_database.wait_for_pending_background_compactions();
}

void register_insertion_benchmarks(bench_config_t const& bench_config,
                                   single_collection_databases_t& single_collection_databases) {

    auto fixed_width_single_collection_database_name = string_t::format(
        "fixed_{}_bytes_single_collection_database", bench_config.single_collection_database_fixed_width.value_size);
    auto variable_width_single_collection_database_name = string_t::format(
        "variable_width_single_collection_database (max: {} bytes)",
        bench_config.single_collection_database_variable_width.value_max_size);

    register_section("<<<Insertion: pulling>>>");
    if (bench_config.single_collection_database_12_bytes.enabled) {
        auto threads_count = bench_config.single_collection_database_12_bytes.threads_count;
        auto population_count = bench_config.single_collection_database_12_bytes.population_count / threads_count;
        register_benchmark("12_bytes_single_collection_database",
                           {0, population_count, time_unit_t::microsecond_k, threads_count}, [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_pulling_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               insert(state, bench_config.single_collection_database_12_bytes,
                                      *single_collection_databases.single_collection_database_12_bytes);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }

    if (bench_config.single_collection_database_44_bytes.enabled) {
        auto threads_count = bench_config.single_collection_database_44_bytes.threads_count;
        auto population_count = bench_config.single_collection_database_44_bytes.population_count / threads_count;
        register_benchmark("44_bytes_single_collection_database",
                           {0, population_count, time_unit_t::microsecond_k, threads_count}, [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_pulling_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               insert(state, bench_config.single_collection_database_44_bytes,
                                      *single_collection_databases.single_collection_database_44_bytes);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
    if (bench_config.single_collection_database_fixed_width.enabled) {
        auto threads_count = bench_config.single_collection_database_fixed_width.threads_count;
        auto population_count = bench_config.single_collection_database_fixed_width.population_count / threads_count;
        register_benchmark(fixed_width_single_collection_database_name,
                           {0, population_count, time_unit_t::microsecond_k, threads_count}, [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_pulling_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               insert(state, bench_config.single_collection_database_fixed_width,
                                      *single_collection_databases.single_collection_database_fixed_width);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
    if (bench_config.single_collection_database_variable_width.enabled) {
        auto threads_count = bench_config.single_collection_database_variable_width.threads_count;
        auto population_count = bench_config.single_collection_database_variable_width.population_count / threads_count;
        register_benchmark(variable_width_single_collection_database_name,
                           {0, population_count, time_unit_t::microsecond_k, threads_count}, [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_pulling_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               insert(state, bench_config.single_collection_database_variable_width,
                                      *single_collection_databases.single_collection_database_variable_width);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
}

void register_single_find_benchmarks(bench_config_t const& bench_config,
                                     single_collection_databases_t& single_collection_databases) {

    auto fixed_width_single_collection_database_name = string_t::format(
        "fixed_{}_bytes_single_collection_database", bench_config.single_collection_database_fixed_width.value_size);
    auto variable_width_single_collection_database_name = string_t::format(
        "variable_width_single_collection_database (max: {} bytes)",
        bench_config.single_collection_database_variable_width.value_max_size);

    register_section("<<<Single find: mmap>>>");
    if (bench_config.single_collection_database_12_bytes.enabled) {
        auto threads_count = bench_config.single_collection_database_12_bytes.threads_count;
        auto population_count = bench_config.single_collection_database_12_bytes.population_count / threads_count;
        register_benchmark("12_bytes_single_collection_database",
                           {0, population_count, time_unit_t::microsecond_k, threads_count}, [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_posix_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               find_only(state, bench_config.single_collection_database_12_bytes,
                                         *single_collection_databases.single_collection_database_12_bytes);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
    if (bench_config.single_collection_database_44_bytes.enabled) {
        auto threads_count = bench_config.single_collection_database_44_bytes.threads_count;
        auto population_count = bench_config.single_collection_database_44_bytes.population_count / threads_count;
        register_benchmark("44_bytes_single_collection_database",
                           {0, population_count, time_unit_t::microsecond_k, threads_count}, [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_posix_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               find_only(state, bench_config.single_collection_database_44_bytes,
                                         *single_collection_databases.single_collection_database_44_bytes);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }

    if (bench_config.single_collection_database_fixed_width.enabled) {
        auto threads_count = bench_config.single_collection_database_fixed_width.threads_count;
        auto population_count = bench_config.single_collection_database_fixed_width.population_count / threads_count;
        register_benchmark(fixed_width_single_collection_database_name,
                           {0, population_count, time_unit_t::microsecond_k, threads_count}, [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_pulling_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               find_only(state, bench_config.single_collection_database_fixed_width,
                                         *single_collection_databases.single_collection_database_fixed_width);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
    if (bench_config.single_collection_database_variable_width.enabled) {
        auto threads_count = bench_config.single_collection_database_variable_width.threads_count;
        auto population_count = bench_config.single_collection_database_variable_width.population_count / threads_count;
        register_benchmark(variable_width_single_collection_database_name,
                           {0, population_count, time_unit_t::microsecond_k, threads_count}, [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_pulling_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               find_only(state, bench_config.single_collection_database_variable_width,
                                         *single_collection_databases.single_collection_database_variable_width);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
}

void register_single_selection_inline_benchmarks(bench_config_t const& bench_config,
                                                 single_collection_databases_t& single_collection_databases) {

    register_section("<<<Single selection: mmap>>>");
    if (bench_config.single_collection_database_12_bytes.enabled) {
        auto threads_count = bench_config.single_collection_database_12_bytes.threads_count;
        auto population_count = bench_config.single_collection_database_12_bytes.population_count / threads_count;
        register_benchmark("12_bytes_single_collection_database",
                           {0, population_count, time_unit_t::microsecond_k, threads_count}, [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_posix_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               select(state, bench_config.single_collection_database_12_bytes,
                                      *single_collection_databases.single_collection_database_12_bytes);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
    if (bench_config.single_collection_database_44_bytes.enabled) {
        auto threads_count = bench_config.single_collection_database_44_bytes.threads_count;
        auto population_count = bench_config.single_collection_database_44_bytes.population_count / threads_count;
        register_benchmark("44_bytes_single_collection_database",
                           {0, population_count, time_unit_t::microsecond_k, threads_count}, [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_posix_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               select(state, bench_config.single_collection_database_44_bytes,
                                      *single_collection_databases.single_collection_database_44_bytes);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
}

void register_single_selection_benchmarks(bench_config_t const& bench_config,
                                          single_collection_databases_t& single_collection_databases) {

    auto fixed_width_single_collection_database_name = string_t::format(
        "fixed_{}_bytes_single_collection_database", bench_config.single_collection_database_fixed_width.value_size);
    auto variable_width_single_collection_database_name = string_t::format(
        "variable_width_single_collection_database (max: {} bytes)",
        bench_config.single_collection_database_variable_width.value_max_size);

    register_section("<<<Single selection: pulling(256)>>>");
    if (bench_config.single_collection_database_fixed_width.enabled) {
        auto threads_count = bench_config.single_collection_database_fixed_width.threads_count;
        auto population_count = bench_config.single_collection_database_fixed_width.population_count / threads_count;
        register_benchmark(fixed_width_single_collection_database_name,
                           {0, population_count, time_unit_t::microsecond_k, threads_count}, [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_pulling_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               select(state, bench_config.single_collection_database_fixed_width,
                                      *single_collection_databases.single_collection_database_fixed_width);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
    if (bench_config.single_collection_database_variable_width.enabled) {
        auto threads_count = bench_config.single_collection_database_variable_width.threads_count;
        auto population_count = bench_config.single_collection_database_variable_width.population_count / threads_count;
        register_benchmark(variable_width_single_collection_database_name,
                           {0, population_count, time_unit_t::microsecond_k, threads_count}, [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_pulling_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               select(state, bench_config.single_collection_database_variable_width,
                                      *single_collection_databases.single_collection_database_variable_width);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
}

void register_batch_selection_benchmarks(bench_config_t const& bench_config,
                                         single_collection_databases_t& single_collection_databases) {

    auto fixed_width_single_collection_database_name = string_t::format(
        "fixed_{}_bytes_single_collection_database", bench_config.single_collection_database_fixed_width.value_size);
    auto variable_width_single_collection_database_name = string_t::format(
        "variable_width_single_collection_database (max: {} bytes)",
        bench_config.single_collection_database_variable_width.value_max_size);

    std::size_t const queue_depth = 256;
    register_section("<<<Single batch selection: pulling(256)>>>");
    if (bench_config.single_collection_database_fixed_width.enabled) {
        auto threads_count = bench_config.single_collection_database_fixed_width.threads_count;
        auto population_count = bench_config.single_collection_database_fixed_width.population_count / threads_count;
        register_benchmark(fixed_width_single_collection_database_name,
                           {0, population_count / queue_depth, time_unit_t::microsecond_k, threads_count},
                           [&, queue_depth](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_pulling_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               batch_select(state, bench_config.single_collection_database_fixed_width,
                                            *single_collection_databases.single_collection_database_fixed_width,
                                            queue_depth);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
    if (bench_config.single_collection_database_variable_width.enabled) {
        auto threads_count = bench_config.single_collection_database_variable_width.threads_count;
        auto population_count = bench_config.single_collection_database_variable_width.population_count / threads_count;
        register_benchmark(variable_width_single_collection_database_name,
                           {0, population_count / queue_depth, time_unit_t::microsecond_k, threads_count},
                           [&, queue_depth](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_pulling_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               batch_select(state, bench_config.single_collection_database_variable_width,
                                            *single_collection_databases.single_collection_database_variable_width,
                                            queue_depth);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
}

void register_scan_inline_benchmarks(bench_config_t const& bench_config,
                                     single_collection_databases_t& single_collection_databases) {

    register_section("<<<Scan: mmap>>>");
    if (bench_config.single_collection_database_12_bytes.enabled) {
        auto threads_count = bench_config.single_collection_database_12_bytes.threads_count;
        register_benchmark("12_bytes_single_collection_database", {0, 1, time_unit_t::microsecond_k, threads_count},
                           [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_posix_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               scan(state, *single_collection_databases.single_collection_database_12_bytes);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
    if (bench_config.single_collection_database_44_bytes.enabled) {
        auto threads_count = bench_config.single_collection_database_44_bytes.threads_count;
        register_benchmark("44_bytes_single_collection_database", {0, 1, time_unit_t::microsecond_k, threads_count},
                           [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_posix_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               scan(state, *single_collection_databases.single_collection_database_44_bytes);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
}

void register_scan_benchmarks(bench_config_t const& bench_config,
                              single_collection_databases_t& single_collection_databases) {

    auto fixed_width_single_collection_database_name = string_t::format(
        "fixed_{}_bytes_single_collection_database", bench_config.single_collection_database_fixed_width.value_size);
    auto variable_width_single_collection_database_name = string_t::format(
        "variable_width_single_collection_database (max: {} bytes)",
        bench_config.single_collection_database_variable_width.value_max_size);

    register_section("<<<Scan: pulling(256)>>>");
    if (bench_config.single_collection_database_fixed_width.enabled) {
        auto threads_count = bench_config.single_collection_database_fixed_width.threads_count;
        register_benchmark(fixed_width_single_collection_database_name,
                           {0, 1, time_unit_t::microsecond_k, threads_count}, [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_pulling_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               scan(state, *single_collection_databases.single_collection_database_fixed_width);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
    if (bench_config.single_collection_database_variable_width.enabled) {
        auto threads_count = bench_config.single_collection_database_variable_width.threads_count;
        register_benchmark(variable_width_single_collection_database_name,
                           {0, 1, time_unit_t::microsecond_k, threads_count}, [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_pulling_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               scan(state, *single_collection_databases.single_collection_database_variable_width);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
}

void register_deletion_benchmarks(bench_config_t const& bench_config,
                                  single_collection_databases_t& single_collection_databases) {

    auto fixed_width_single_collection_database_name = string_t::format(
        "fixed_{}_bytes_single_collection_database", bench_config.single_collection_database_fixed_width.value_size);
    auto variable_width_single_collection_database_name = string_t::format(
        "variable_width_single_collection_database (max: {} bytes)",
        bench_config.single_collection_database_variable_width.value_max_size);

    register_section("<<<Deletion: pulling>>>");
    if (bench_config.single_collection_database_12_bytes.enabled) {
        auto threads_count = bench_config.single_collection_database_12_bytes.threads_count;
        auto population_count = bench_config.single_collection_database_12_bytes.population_count / threads_count;
        register_benchmark("12_bytes_single_collection_database",
                           {0, population_count, time_unit_t::microsecond_k, threads_count}, [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_pulling_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               remove(state, bench_config.single_collection_database_12_bytes,
                                      *single_collection_databases.single_collection_database_12_bytes);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
    if (bench_config.single_collection_database_44_bytes.enabled) {
        auto threads_count = bench_config.single_collection_database_44_bytes.threads_count;
        auto population_count = bench_config.single_collection_database_44_bytes.population_count / threads_count;
        register_benchmark("44_bytes_single_collection_database",
                           {0, population_count, time_unit_t::microsecond_k, threads_count}, [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_pulling_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               remove(state, bench_config.single_collection_database_44_bytes,
                                      *single_collection_databases.single_collection_database_44_bytes);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
    if (bench_config.single_collection_database_fixed_width.enabled) {
        auto threads_count = bench_config.single_collection_database_fixed_width.threads_count;
        auto population_count = bench_config.single_collection_database_fixed_width.population_count / threads_count;
        register_benchmark(fixed_width_single_collection_database_name,
                           {0, population_count, time_unit_t::microsecond_k, threads_count}, [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_pulling_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               remove(state, bench_config.single_collection_database_fixed_width,
                                      *single_collection_databases.single_collection_database_fixed_width);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
    if (bench_config.single_collection_database_variable_width.enabled) {
        auto threads_count = bench_config.single_collection_database_variable_width.threads_count;
        auto population_count = bench_config.single_collection_database_variable_width.population_count / threads_count;
        register_benchmark(variable_width_single_collection_database_name,
                           {0, population_count, time_unit_t::microsecond_k, threads_count}, [&](bm::State& state) {
                               if (state.thread_index() == 0) {
                                   initialize_pulling_disk_router(bench_config.folder);
                                   single_collection_databases.open();
                               }
                               remove(state, bench_config.single_collection_database_variable_width,
                                      *single_collection_databases.single_collection_database_variable_width);
                               if (state.thread_index() == 0) single_collection_databases.close();
                           });
    }
}

void register_benchmarks(bench_config_t const& bench_config,
                         single_collection_databases_t& single_collection_databases) {

    register_insertion_benchmarks(bench_config, single_collection_databases);

    register_single_find_benchmarks(bench_config, single_collection_databases);

    register_single_selection_inline_benchmarks(bench_config, single_collection_databases);
    register_single_selection_benchmarks(bench_config, single_collection_databases);

    register_batch_selection_benchmarks(bench_config, single_collection_databases);

    register_scan_inline_benchmarks(bench_config, single_collection_databases);
    register_scan_benchmarks(bench_config, single_collection_databases);
}

void configure_bench_by_workloads(bench_config_t& bench_config, st::basic_vector<workload_t> const& workloads) {

    auto find_workload = [&](string_t const& name, workload_t& workload) {
        for (std::size_t index = 0; index < workloads.size(); ++index) {
            if (name == workloads[index].name()) {
                workload = workloads[index];
                return true;
            }
        }
        return false;
    };

    workload_t workload("");

    bench_config.single_collection_database_12_bytes.enabled = find_workload("12_bytes", workload);
    if (bench_config.single_collection_database_12_bytes.enabled) {
        bench_config.single_collection_database_12_bytes.threads_count = workload.get_value<std::size_t>("threads");
        bench_config.single_collection_database_12_bytes.enabled = workload.get_value<bool>("enabled");
        bench_config.single_collection_database_12_bytes.population_count = workload.get_value<std::size_t>(
            "population_count");
        bench_config.single_collection_database_12_bytes.value_size = 8;
        bench_config.single_collection_database_12_bytes.value_max_size = 0;
        bench_config.single_collection_database_12_bytes.cache_limit = workload.get_value<std::size_t>("cache_limit");
        bench_config.single_collection_database_12_bytes.memory_runs_max_bytes = workload.get_value<std::size_t>(
            "memory_runs_max_bytes");
        bench_config.single_collection_database_12_bytes.memory_runs_max_deltas = workload.get_value<std::size_t>(
            "memory_runs_max_deltas");
        bench_config.single_collection_database_12_bytes.level_enlarge_factor = workload.get_value<std::size_t>(
            "level_enlarge_factor");
    }

    bench_config.single_collection_database_44_bytes.enabled = find_workload("44_bytes", workload);
    if (bench_config.single_collection_database_44_bytes.enabled) {
        bench_config.single_collection_database_44_bytes.threads_count = workload.get_value<std::size_t>("threads");
        bench_config.single_collection_database_44_bytes.enabled = workload.get_value<bool>("enabled");
        bench_config.single_collection_database_44_bytes.population_count = workload.get_value<std::size_t>(
            "population_count");
        bench_config.single_collection_database_44_bytes.value_size = 64;
        bench_config.single_collection_database_44_bytes.value_max_size = 0;
        bench_config.single_collection_database_44_bytes.cache_limit = workload.get_value<std::size_t>("cache_limit");
        bench_config.single_collection_database_44_bytes.memory_runs_max_bytes = workload.get_value<std::size_t>(
            "memory_runs_max_bytes");
        bench_config.single_collection_database_44_bytes.memory_runs_max_deltas = workload.get_value<std::size_t>(
            "memory_runs_max_deltas");
        bench_config.single_collection_database_44_bytes.level_enlarge_factor = workload.get_value<std::size_t>(
            "level_enlarge_factor");
    }

    bench_config.single_collection_database_fixed_width.enabled = find_workload("fixed_width", workload);
    if (bench_config.single_collection_database_fixed_width.enabled) {
        bench_config.single_collection_database_fixed_width.threads_count = workload.get_value<std::size_t>("threads");
        bench_config.single_collection_database_fixed_width.enabled = workload.get_value<bool>("enabled");
        bench_config.single_collection_database_fixed_width.population_count = workload.get_value<std::size_t>(
            "population_count");
        bench_config.single_collection_database_fixed_width.value_size = workload.get_value<std::size_t>("value_size");
        bench_config.single_collection_database_fixed_width.value_max_size = 0;
        bench_config.single_collection_database_fixed_width.cache_limit = workload.get_value<std::size_t>(
            "cache_limit");
        bench_config.single_collection_database_fixed_width.memory_runs_max_bytes = workload.get_value<std::size_t>(
            "memory_runs_max_bytes");
        bench_config.single_collection_database_fixed_width.memory_runs_max_deltas = workload.get_value<std::size_t>(
            "memory_runs_max_deltas");
        bench_config.single_collection_database_fixed_width.level_enlarge_factor = workload.get_value<std::size_t>(
            "level_enlarge_factor");
    }

    bench_config.single_collection_database_variable_width.enabled = find_workload("variable_width", workload);
    if (bench_config.single_collection_database_variable_width.enabled) {
        bench_config.single_collection_database_variable_width.threads_count = workload.get_value<std::size_t>(
            "threads");
        bench_config.single_collection_database_variable_width.enabled = workload.get_value<bool>("enabled");
        bench_config.single_collection_database_variable_width.population_count = workload.get_value<std::size_t>(
            "population_count");
        bench_config.single_collection_database_variable_width.value_size = 0;
        bench_config.single_collection_database_variable_width.value_max_size = workload.get_value<std::size_t>(
            "value_max_size");
        bench_config.single_collection_database_variable_width.cache_limit = workload.get_value<std::size_t>(
            "cache_limit");
        bench_config.single_collection_database_variable_width.memory_runs_max_bytes = workload.get_value<std::size_t>(
            "memory_runs_max_bytes");
        bench_config.single_collection_database_variable_width.memory_runs_max_deltas = workload.get_value<std::size_t>(
            "memory_runs_max_deltas");
        bench_config.single_collection_database_variable_width.level_enlarge_factor = workload.get_value<std::size_t>(
            "level_enlarge_factor");
    }
}

int main(int argc, char** argv) {

    options_t options;
    std::array<flag_t, 1> const flags {{
        {"--directory", "path", "directory the single-collection databases and the log are written in",
         &options.directory},
    }};
    arguments_t const parsed = parse_arguments(argc, argv, flags, {});
    if (parsed.status == argument_status_t::help_k) {
        print_usage("ustore_bench_nvme_single_collection_database --directory <path>", flags);
        return 0;
    }
    if (parsed.status != argument_status_t::parsed_k) {
        fmt::println(stderr, "{}: {}", parsed.argument, describe(parsed.status));
        return 1;
    }
    if (options.directory.empty()) {
        fmt::println(stderr, "--directory is required");
        return 1;
    }

    bench_config_t bench_config;
    bench_config.folder = string_t::format("{}/", options.directory);

    countdown_t muted(2);
    make_path(bench_config.folder, muted);
    auto log_file = bench_config.folder.appended("single_collection_database.log");
    remove_file(log_file, muted);

    resources = std::make_shared<resources_t>(1'000'000'000'000ull);
    resources->logger = std::make_shared<logger_t>(log_file.c_str());
    initialize_posix_disk_router(bench_config.folder);

    // The legacy harness sees only the program name, as every flag is parsed above
    project_t project(1, argv);
    configure_bench_by_workloads(bench_config, project_workloads(project));

    single_collection_databases_t single_collection_databases(bench_config, resources);
    register_benchmarks(bench_config, single_collection_databases);

    start_benchmark(project);
    make_report(project);

    resources->fibers->stop_and_shutdown();
    resources->disk_router.reset();

    return 0;
}
