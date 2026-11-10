/**
 *  @file bench/compaction.cpp
 *  @author Ash Vardanian
 *  @date November 8, 2026
 *  @brief Times one full compaction of the same overlapping level-0 runs in RocksDB and in the NVMe
 *      engine, the latter merging on host threads or on a GPU.
 *
 *  The load writes random 16-byte keys with fixed-width values in batches while no merge runs:
 *  RocksDB through @c disable_auto_compactions in its options file, and NVMe by stopping half a
 *  memory run short of the level-0 run count that starts a merge. The timed
 *  @c ustore_database_compact then flushes what memory holds and merges every run into one sorted
 *  level, after the page cache dropped every file of the database. Sampled keys are read back.
 *
 *  Beside the flags of @c bench/harness.hpp, it takes:
 *
 *  @verbatim
 *  Flag              Default   Meaning
 *  --engine          nvme      Engine to open: nvme or rocksdb
 *  --compactor       cpu       Where NVMe merges: cpu or gpu
 *  --directory       Required  An empty directory the database fills, unless --open names a raw address
 *  --open            None      A raw slot or spdk:// address NVMe opens instead, like spdk://0000:6e:00.0/1@256GiB
 *  --rocksdb-subcompactions 1 Threads a RocksDB manual compaction splits across
 *  --threads         0         Threads of the executor NVMe opens and compacts on, blocking, or 0 for none
 *  --runs            16        Level-0 runs the merge takes, and the NVMe level enlarge factor
 *  --run-records     400000    Records of every level-0 run
 *  --value-size      100       Bytes of every value, over 44 so NVMe keeps them fixed-width beside the keys
 *  --batch-size      4096      Records per write call
 *  --samples         100000    Keys read back after the merge
 *  --write-limit     8GiB      Bytes the process may write before it stops
 *  @endverbatim
 */
#include <cstdint> // `std::uintmax_t`
#include <cstdio>  // `std::fopen`, `std::fscanf`, `std::fread`, `std::fwrite`, `std::fclose`
#include <cstring> // `std::memcpy`, `std::memcmp`

#include <sys/resource.h> // `getrusage`
#include <fcntl.h>        // `open`, `posix_fadvise`
#include <unistd.h>       // `close`, `fdatasync`

#include <filesystem>   // `std::filesystem::recursive_directory_iterator`, `std::filesystem::file_size`
#include <system_error> // `std::error_code`

#include "ustore/blobs.h"
#include "ustore/db.h"

#include "harness.hpp"

namespace unum::ustore::bench {

#pragma region Settings

inline constexpr std::array<std::string_view, 2> engine_names_k {"nvme", "rocksdb"};
inline constexpr std::array<std::string_view, 2> compactor_names_k {"cpu", "gpu"};

/** Command-line options of the benchmark, each field set by the flag of the same name. */
struct settings_t {
    shared_settings_t shared;
    choice_t engine {engine_names_k};
    choice_t compactor {compactor_names_k};
    std::string_view directory;
    std::string_view open;
    count_t rocksdb_subcompactions {.value = 1, .minimum = 1, .maximum = 16};
    count_t threads {.value = 0, .minimum = 0};
    count_t runs {.value = 16, .minimum = 2, .maximum = 64};
    count_t run_records {.value = 400'000, .minimum = 16, .maximum = 1024 * 1024};
    count_t value_size {.value = 100, .minimum = 45};
    count_t batch_size {4096};
    count_t samples {100'000};
    bytes_t write_limit {std::size_t {8} << 30};

    [[nodiscard]] bool on_rocksdb() const noexcept { return engine.selected == 1; }
    [[nodiscard]] bool on_gpu() const noexcept { return compactor.selected == 1; }

    /** Records of the load: every run but the last whole, and half of the last, which
     *  the compaction flushes. */
    [[nodiscard]] std::size_t records() const noexcept {
        return (runs.value - 1) * run_records.value + run_records.value / 2;
    }
};

#pragma endregion Settings

#pragma region Process Counters

/** Bytes the process read from and wrote to storage, and the CPU time of all its threads. */
struct process_counters_t {
    std::uint64_t read_bytes = 0, write_bytes = 0;
    double cpu_seconds = 0;

    [[nodiscard]] static process_counters_t now() noexcept {
        process_counters_t counters;
        if (std::FILE* const file = std::fopen("/proc/self/io", "r")) {
            char name[32];
            unsigned long long value = 0;
            while (std::fscanf(file, "%31s %llu", name, &value) == 2) {
                if (std::string_view {name} == "read_bytes:") counters.read_bytes = value;
                if (std::string_view {name} == "write_bytes:") counters.write_bytes = value;
            }
            std::fclose(file);
        }
        rusage usage {};
        getrusage(RUSAGE_SELF, &usage);
        counters.cpu_seconds = static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
                               static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
        return counters;
    }
};

#pragma endregion Process Counters

#pragma region Records

/** The key of record @p index, two words of the seeded stream. */
[[nodiscard]] ustore_key_t key_of(std::uint32_t seed, std::size_t index) noexcept {
    std::array<std::uint64_t, 2> const words {stream_key(seed, "keys", 2 * index),
                                              stream_key(seed, "keys", 2 * index + 1)};
    ustore_key_t key;
    std::memcpy(&key, words.data(), sizeof(key));
    return key;
}

/** Fills @p value with the bytes record @p index holds, derived from its key so a read
 *  can be checked. */
void fill_value(std::uint32_t seed, std::size_t index, std::span<std::byte> value) noexcept {
    std::uint64_t const origin = stream_key(seed, "values", index);
    for (std::size_t offset = 0; offset < value.size(); offset += sizeof(std::uint64_t)) {
        std::uint64_t const word = mix(origin + offset);
        std::memcpy(value.data() + offset, &word, fu::min_of(sizeof(word), value.size() - offset));
    }
}

#pragma endregion Records

#pragma region Engines

/** Opens the database @p settings name, answering a reason when it cannot. */
[[nodiscard]] char const* open_database(settings_t const& settings, ustore_executor_t executor,
                                        ustore_executor_t compactions, ustore_database_t& database) {
    ustore_outcome_t outcome {};
    if (settings.on_rocksdb()) {
        ustore_database_open_rocksdb_t open {
            .database = &database,
            .outcome = &outcome,
            .directory = {reinterpret_cast<ustore_byte_t const*>(settings.directory.data()),
                          settings.directory.size()}};
        ustore_database_open_rocksdb(&open);
        return outcome.status == ustore_status_success_k ? nullptr : outcome.message;
    }
    std::string const manifest = settings.open.empty() ? fmt::format("{}/compaction.ustore.json", settings.directory)
                                                       : std::string {settings.open};
    ustore_database_open_nvme_t open {
        .database = &database,
        .executor = executor,
        .outcome = &outcome,
        .directory = {reinterpret_cast<ustore_byte_t const*>(manifest.data()), manifest.size()},
        .compactions = compactions,
        .collection_memory_runs_max_bytes = std::size_t {256} << 20,
        .collection_memory_runs_max_deltas = settings.run_records.value,
        .collection_level_enlarge_factor = settings.runs.value,
        .collection_value_size = settings.value_size.value};
    ustore_database_open_nvme(&open);
    return outcome.status == ustore_status_success_k ? nullptr : outcome.message;
}

/** Whether @c name_collection finds an existing collection or creates a new one. */
enum class collection_naming_t : bool { find_k, create_k };

/** Finds the collection named @p name, or creates it under @c collection_naming_t::create_k,
 *  answering a reason when it cannot. */
[[nodiscard]] char const* name_collection(ustore_database_t database, std::string_view name, collection_naming_t naming,
                                          ustore_collection_id_t& collection) {
    ustore_outcome_t outcome {};
    ustore_bytes_view_t const spelled {reinterpret_cast<ustore_byte_t const*>(name.data()), name.size()};
    if (naming == collection_naming_t::create_k) {
        ustore_collection_create_t created {.database = database,
                                            .outcome = &outcome,
                                            .name = spelled,
                                            .modality = ustore_modality_blobs_k,
                                            .collection = &collection};
        ustore_collection_create(&created);
        return outcome.status == ustore_status_success_k ? nullptr : outcome.message;
    }
    ustore_arena_t arena = nullptr;
    ustore_arena_init_t init {.arena = &arena, .outcome = &outcome};
    ustore_arena_init(&init);
    if (outcome.status != ustore_status_success_k) return outcome.message;
    std::uint32_t const offsets[2] {0, static_cast<std::uint32_t>(name.size())};
    ustore_collection_id_t const* found = nullptr;
    ustore_collection_find_t find {
        .database = database,
        .outcome = &outcome,
        .count = 1,
        .names = {.layout = ustore_strings_u32_tape_k,
                  .data = {.u32_tape = {reinterpret_cast<std::uint8_t const*>(name.data()), offsets}}},
        .collections = &found,
        .arena = arena};
    ustore_collection_find(&find);
    if (outcome.status == ustore_status_success_k) collection = *found;
    ustore_arena_free(arena);
    return outcome.status == ustore_status_success_k ? nullptr : outcome.message;
}

/** Sets every line of @p options naming @p name to @p value, answering how many it set. */
std::size_t set_rocksdb_option(std::string& options, std::string_view name, std::string_view value) {
    std::size_t replaced = 0;
    std::string const key = fmt::format("  {}=", name);
    for (std::size_t found = options.find(key); found != std::string::npos; found = options.find(key, found + 1)) {
        std::size_t const begin = found + key.size(), end = options.find('\n', begin);
        options.replace(begin, end - begin, value), ++replaced;
    }
    return replaced;
}

/** Holds back every automatic compaction of RocksDB and lets a manual one split across
 *  @p subcompactions threads, by rewriting its newest options file, which the next open loads,
 *  answering a reason when it cannot. */
[[nodiscard]] char const* configure_rocksdb(std::string_view directory, std::size_t subcompactions) {
    std::filesystem::path newest;
    for (std::filesystem::directory_entry const& entry : std::filesystem::directory_iterator {directory})
        if (entry.path().filename().string().starts_with("OPTIONS-") && entry.path() > newest) newest = entry.path();
    if (newest.empty()) return "RocksDB wrote no options file";
    std::error_code size_error;
    std::uintmax_t const options_bytes = std::filesystem::file_size(newest, size_error);
    std::FILE* const file = size_error ? nullptr : std::fopen(newest.c_str(), "rb");
    if (!file) return "The options file would not open";
    std::string options(static_cast<std::size_t>(options_bytes), '\0');
    options.resize(std::fread(options.data(), 1, options.size(), file));
    std::fclose(file);
    std::size_t const families = set_rocksdb_option(options, "disable_auto_compactions", "true");
    if (families == 0) return "The options file names no automatic compactions";
    // Subcompactions run on the background pool, which one flush thread shares
    if (set_rocksdb_option(options, "max_subcompactions", fmt::format("{}", subcompactions)) != 1 ||
        set_rocksdb_option(options, "max_background_jobs", fmt::format("{}", subcompactions + 1)) != 1)
        return "The options file names no subcompactions or background jobs";
    std::FILE* const rewritten = std::fopen(newest.c_str(), "wb");
    if (!rewritten) return "The options file would not open for writing";
    std::fwrite(options.data(), 1, options.size(), rewritten);
    std::fclose(rewritten);
    fmt::println("- rocksdb_options: {} column families with disable_auto_compactions=true, max_subcompactions={}, " //
                 "max_background_jobs={} in {}",
                 families, subcompactions, subcompactions + 1, newest.filename().string());
    return nullptr;
}

/** Syncs every file under @p directory and drops it from the page cache, answering its
 *  bytes on disk. */
std::uint64_t drop_cached(std::string_view directory) noexcept {
    std::uint64_t bytes = 0;
    std::error_code error;
    for (std::filesystem::directory_entry const& entry :
         std::filesystem::recursive_directory_iterator {directory, error}) {
        if (!entry.is_regular_file(error)) continue;
        bytes += entry.file_size(error);
        int const file = ::open(entry.path().c_str(), O_RDONLY);
        if (file < 0) continue;
        ::fdatasync(file);
        ::posix_fadvise(file, 0, 0, POSIX_FADV_DONTNEED);
        ::close(file);
    }
    return bytes;
}

#pragma endregion Engines

#pragma region Phases

/** Writes every record of @p settings in batches, refusing to pass the write limit, answering a
 *  reason when it stops. */
[[nodiscard]] char const* load(settings_t const& settings, ustore_database_t database,
                               ustore_collection_id_t collection) {
    std::size_t const records = settings.records(), batch = settings.batch_size.value;
    std::size_t const value_size = settings.value_size.value;
    st::vector<ustore_key_t> keys;
    st::vector<std::byte> tape;
    st::vector<std::uint32_t> offsets;
    if (st::failed(keys.resize(batch)) || st::failed(tape.resize(batch * value_size)) ||
        st::failed(offsets.resize(batch + 1)))
        return "Failed to allocate a batch";
    for (std::size_t index = 0; index <= batch; ++index)
        offsets[index] = static_cast<std::uint32_t>(index * value_size);

    process_counters_t const before = process_counters_t::now();
    time_point_t const start = steady_clock_t::now();
    ustore_outcome_t outcome {};
    for (std::size_t first = 0; first < records; first += batch) {
        std::size_t const count = fu::min_of(batch, records - first);
        for (std::size_t row = 0; row != count; ++row) {
            keys[row] = key_of(settings.shared.seed.value, first + row);
            fill_value(settings.shared.seed.value, first + row, {tape.data() + row * value_size, value_size});
        }
        ustore_blobs_write_t write {
            .database = database,
            .outcome = &outcome,
            .count = count,
            .collections = {&collection, 0},
            .keys = {keys.data(), sizeof(ustore_key_t)},
            .values = {.layout = ustore_strings_u32_tape_k,
                       .data = {.u32_tape = {reinterpret_cast<std::uint8_t const*>(tape.data()), offsets.data()}}}};
        ustore_blobs_write(&write);
        if (outcome.status != ustore_status_success_k) return outcome.message;
        if (process_counters_t::now().write_bytes - before.write_bytes > settings.write_limit.value)
            return "The load passed the write limit";
    }
    double const seconds = std::chrono::duration<double>(steady_clock_t::now() - start).count();
    process_counters_t const after = process_counters_t::now();
    fmt::println("- load: records {} seconds {:.3f} write_bytes {} cpu_seconds {:.2f}", records, seconds,
                 after.write_bytes - before.write_bytes, after.cpu_seconds - before.cpu_seconds);
    return nullptr;
}

/** Reads back @p settings' sampled records after the merge, answering a reason on a missing
 *  or changed value. */
[[nodiscard]] char const* check(settings_t const& settings, ustore_database_t database,
                                ustore_collection_id_t collection) {
    std::size_t const samples = settings.samples.value, value_size = settings.value_size.value;
    st::vector<ustore_key_t> keys;
    st::vector<std::size_t> indices;
    st::vector<std::byte> expected;
    if (st::failed(keys.resize(samples)) || st::failed(indices.resize(samples)) ||
        st::failed(expected.resize(value_size)))
        return "Failed to allocate the samples";
    splitmix64_t random {settings.shared.seed.value};
    for (std::size_t sample = 0; sample != samples; ++sample) {
        indices[sample] = static_cast<std::size_t>(random.below(settings.records()));
        keys[sample] = key_of(settings.shared.seed.value, indices[sample]);
    }
    ustore_outcome_t outcome {};
    ustore_arena_t arena = nullptr;
    ustore_arena_init_t init {.arena = &arena, .outcome = &outcome};
    ustore_arena_init(&init);
    if (outcome.status != ustore_status_success_k) return outcome.message;
    ustore_strings_view_t values {};
    ustore_blobs_read_t read {.database = database,
                              .outcome = &outcome,
                              .count = samples,
                              .collections = {&collection, 0},
                              .keys = {keys.data(), sizeof(ustore_key_t)},
                              .values = &values,
                              .arena = arena};
    ustore_blobs_read(&read);
    char const* reason = outcome.status == ustore_status_success_k ? nullptr : outcome.message;
    for (std::size_t sample = 0; !reason && sample != samples; ++sample) {
        fill_value(settings.shared.seed.value, indices[sample], expected);
        bool const wide = values.layout == ustore_strings_u64_tape_k;
        std::uint64_t const begin = wide ? values.data.u64_tape.offsets[sample] : values.data.u32_tape.offsets[sample];
        std::uint64_t const end = wide ? values.data.u64_tape.offsets[sample + 1]
                                       : values.data.u32_tape.offsets[sample + 1];
        std::uint8_t const* const data = wide ? values.data.u64_tape.data : values.data.u32_tape.data;
        if (end - begin != value_size || std::memcmp(data + begin, expected.data(), value_size) != 0)
            reason = "A sampled value is missing or changed after the merge";
    }
    ustore_arena_free(arena);
    if (!reason) fmt::println("- check: {} sampled values match", samples);
    return reason;
}

#pragma endregion Phases

/** Runs the benchmark, answering a reason when it stops early. */
[[nodiscard]] char const* run(settings_t const& settings) {
    if (settings.on_rocksdb() && settings.on_gpu()) return "RocksDB merges on the CPU alone";
    if (settings.on_rocksdb() && !settings.open.empty()) return "RocksDB opens a directory alone";
    if (settings.on_rocksdb() && settings.threads.value != 0) return "RocksDB compacts on its own threads";
    if (settings.open.empty()) {
        if (std::filesystem::exists(settings.directory) && !std::filesystem::is_empty(settings.directory))
            return "The directory must be empty";
        std::filesystem::create_directories(settings.directory);
    }

    ustore_executor_t executor = nullptr;
    ustore_outcome_t outcome {};
    if (settings.threads.value != 0) {
        ustore_executor_for_nvme_t spawn {.executor = &executor,
                                          .outcome = &outcome,
                                          .threads = settings.threads.value};
        ustore_executor_for_nvme(&spawn);
        if (outcome.status != ustore_status_success_k) return outcome.message;
    }
    ustore_executor_t compactions = nullptr;
    if (settings.on_gpu()) {
        ustore_executor_for_vram_t spawn {.executor = &compactions,
                                          .outcome = &outcome,
                                          .device = 0,
                                          .gds = ustore_gds_compatibility_k};
        ustore_executor_for_vram(&spawn);
        if (outcome.status != ustore_status_success_k) {
            if (executor) ustore_executor_free(executor);
            return outcome.message;
        }
    }
    ustore_database_t database = nullptr;
    ustore_collection_id_t collection {};
    std::string_view constexpr name = "compaction";
    char const* reason = open_database(settings, executor, compactions, database);
    if (!reason) reason = name_collection(database, name, collection_naming_t::create_k, collection);
    if (!reason && settings.on_rocksdb()) {
        ustore_database_free(database);
        database = nullptr;
        reason = configure_rocksdb(settings.directory, settings.rocksdb_subcompactions.value);
        if (!reason) reason = open_database(settings, executor, compactions, database);
        if (!reason) reason = name_collection(database, name, collection_naming_t::find_k, collection);
    }
    if (!reason) reason = load(settings, database, collection);

    if (!reason) {
        // Raw slots and SPDK namespaces bypass the page cache, so only a directory has
        // anything to drop
        std::uint64_t const disk_bytes = settings.open.empty() ? drop_cached(settings.directory) : 0;
        std::uint64_t const records = settings.records();
        std::uint64_t const bytes = records * (sizeof(ustore_key_t) + settings.value_size.value);
        process_counters_t const before = process_counters_t::now();
        time_point_t const start = steady_clock_t::now();
        ustore_database_compact_t compact {.database = database, .executor = executor, .outcome = &outcome};
        ustore_database_compact(&compact);
        double const seconds = std::chrono::duration<double>(steady_clock_t::now() - start).count();
        process_counters_t const after = process_counters_t::now();
        if (outcome.status != ustore_status_success_k) reason = outcome.message;
        else
            fmt::println("- compaction: engine {} compactor {} threads {} records {} bytes {} seconds {:.3f} gbps " //
                         "{:.3f} cpu_seconds {:.2f} read_bytes {} write_bytes {} disk_bytes_before {}",
                         engine_names_k[settings.engine.selected], compactor_names_k[settings.compactor.selected],
                         settings.threads.value, records, bytes, seconds, static_cast<double>(bytes) / seconds / 1e9,
                         after.cpu_seconds - before.cpu_seconds, after.read_bytes - before.read_bytes,
                         after.write_bytes - before.write_bytes, disk_bytes);
    }
    if (!reason) reason = check(settings, database, collection);
    ustore_database_free(database);
    if (compactions) ustore_executor_free(compactions);
    if (executor) ustore_executor_free(executor);
    return reason;
}

} // namespace unum::ustore::bench

int main(int argc, char** argv) {
    using namespace unum::ustore::bench;
    settings_t settings;
    flag_t const flags[] {
        {"--engine", "name", "engine to open: nvme or rocksdb", &settings.engine},
        {"--compactor", "name", "where nvme merges: cpu or gpu", &settings.compactor},
        {"--directory", "path", "an empty directory the database fills", &settings.directory},
        {"--open", "address", "a raw slot or spdk:// address nvme opens instead of a directory", &settings.open},
        {"--rocksdb-subcompactions", "count", "threads a rocksdb manual compaction splits across",
         &settings.rocksdb_subcompactions},
        {"--threads", "count", "threads of the executor nvme opens and compacts on, blocking, or 0 for none",
         &settings.threads},
        {"--runs", "count", "level-0 runs the merge takes, and the nvme level enlarge factor", &settings.runs},
        {"--run-records", "count", "records of every level-0 run", &settings.run_records},
        {"--value-size", "bytes", "bytes of every value", &settings.value_size},
        {"--batch-size", "count", "records per write call", &settings.batch_size},
        {"--samples", "count", "keys read back after the merge", &settings.samples},
        {"--write-limit", "bytes", "bytes the process may write before it stops", &settings.write_limit},
    };
    read_settings(argc, argv, settings.shared, flags);
    if (settings.directory.empty() && settings.open.empty()) {
        fmt::println(stderr, "--directory or --open is required");
        return 1;
    }
    if (char const* const reason = run(settings)) {
        fmt::println(stderr, "compaction: {}", reason);
        return 1;
    }
    return 0;
}
