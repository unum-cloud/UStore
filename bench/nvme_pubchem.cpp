/**
 *  @file bench/nvme_pubchem.cpp
 *  @author Ash Vardanian
 *  @date November 4, 2026
 *  @brief Ingests PubChem molecules into an NVMe database, then measures what the ingest cost
 *      the drives and how fast and how deep point reads go.
 *
 *  Every row of the input becomes a blob keyed by the molecule's InChIKey UUID, holding its
 *  fingerprints back to back, and a string, its SMILES, mapping onto that UUID. The input is one
 *  Arrow IPC stream whose batches carry @c key as 16-byte fixed-size binary, @c smiles as binary,
 *  and @c fingerprints as fixed-size binary, read through the project's own codec. The database is
 *  opened through the C ABI alone, so one source measures every layout the library has shipped.
 *
 *  Beside the flags of @c bench/harness.hpp, it takes:
 *
 *  @verbatim
 *  Flag                  Default   Meaning
 *  --input               Required  The Arrow IPC stream of rows
 *  --open                Required  The directory or manifest the NVMe open takes
 *  --directories         None      Data directories every run may land on
 *  --devices             None      Block devices whose /proc/diskstats writes the ingest is charged
 *  --rows                10000000  Rows ingested from the front of the input
 *  --batch-size          4096      Rows per write call
 *  --threads             8         Threads ingesting and reading
 *  @endverbatim
 */
#include <cstdio>  // `std::fopen`, `std::fgets`
#include <cstring> // `std::memcpy`

#include <sys/inotify.h> // `inotify_init1`, `inotify_add_watch`
#include <sys/mman.h>    // `mmap`
#include <sys/stat.h>    // `fstat`
#include <sys/statvfs.h> // `statvfs`
#include <fcntl.h>       // `open`
#include <unistd.h>      // `close`

#include <atomic>     // `std::atomic`
#include <filesystem> // `std::filesystem::is_directory`
#include <thread>     // `std::thread`

#include "ustore/blobs.h"
#include "ustore/db.h"
#include "ustore/strings.h"

#include "arrow_ipc.hpp" // `ipc_read_prefix`, `ipc_read_message`, `ipc_read_schema`, `ipc_read_batch`
#include "harness.hpp"

namespace unum::ustore::bench {

#pragma region Settings

/** Command-line options of the benchmark, each field set by the flag of the same name. */
struct settings_t : shared_settings_t {
    std::string_view input;
    std::string_view open;
    texts_t directories;
    texts_t devices;
    count_t rows {10'000'000};
    count_t batch_size {.value = 4096, .minimum = 1};
    count_t threads {.value = 8, .minimum = 1};
};

#pragma endregion Settings

#pragma region Input

/** The rows of the input, viewing its mapped batches: a key, a SMILES and fingerprints each. */
struct molecules_t {
    st::vector<ustore_key_t> keys;
    st::vector<std::string_view> smiles;
    st::vector<std::span<std::byte const>> fingerprints;
    std::span<std::byte const> mapped;

    /** Bytes of the keys, the SMILES and the fingerprints of the first @p rows rows. */
    [[nodiscard]] std::uint64_t bytes(std::size_t rows) const noexcept {
        std::uint64_t total = 0;
        for (std::size_t row = 0; row != rows; ++row)
            total += 2 * sizeof(ustore_key_t) + smiles[row].size() + fingerprints[row].size();
        return total;
    }
};

/** Maps @p path and views the first @p rows rows of its batches, or answers why it cannot. */
[[nodiscard]] char const* load_molecules(std::string_view path, std::size_t rows, molecules_t& into) {
    std::string const terminated {path};
    int const file = ::open(terminated.c_str(), O_RDONLY);
    if (file < 0) return "the input does not open";
    struct stat status {};
    if (::fstat(file, &status) != 0) return ::close(file), "the input does not stat";
    void* const mapped = ::mmap(nullptr, static_cast<std::size_t>(status.st_size), PROT_READ, MAP_PRIVATE, file, 0);
    ::close(file);
    if (mapped == MAP_FAILED) return "the input does not map";
    into.mapped = {static_cast<std::byte const*>(mapped), static_cast<std::size_t>(status.st_size)};

    std::array<ipc_field_t, ipc_fields_max_k> fields;
    std::array<ipc_key_value_t, 16> metadata;
    ipc_schema_t schema;
    std::array<ipc_node_t, ipc_fields_max_k> nodes;
    std::array<std::span<std::byte const>, ipc_buffers_max_k> buffers;
    std::size_t key_field = 0, smiles_field = 0, fingerprints_field = 0;
    bool schema_read = false;
    for (std::size_t at = 0; at < into.mapped.size() && into.keys.size() < rows;) {
        std::uint32_t metadata_bytes = 0;
        std::size_t prefix_bytes = 0;
        if (ipc_read_prefix(into.mapped.subspan(at), metadata_bytes, prefix_bytes) != ipc_status_t::success_k)
            return "a message prefix does not read";
        if (metadata_bytes == 0) break;
        std::span<std::byte const> const message_bytes = into.mapped.subspan(at + prefix_bytes, metadata_bytes);
        ipc_message_t message;
        if (ipc_read_message(message_bytes, message) != ipc_status_t::success_k) return "a message does not read";
        std::span<std::byte const> const body = into.mapped.subspan(at + prefix_bytes + metadata_bytes,
                                                                    static_cast<std::size_t>(message.body_bytes));
        at += prefix_bytes + metadata_bytes + static_cast<std::size_t>(message.body_bytes);
        if (!schema_read) {
            if (ipc_read_schema(message, fields, metadata, schema) != ipc_status_t::success_k)
                return "the schema does not read";
            for (std::size_t index = 0; index != schema.fields.size(); ++index) {
                if (schema.fields[index].name == "key") key_field = index;
                if (schema.fields[index].name == "smiles") smiles_field = index;
                if (schema.fields[index].name == "fingerprints") fingerprints_field = index;
            }
            schema_read = true;
            continue;
        }
        ipc_batch_t batch;
        if (ipc_read_batch(message, body, schema, nodes, buffers, batch) != ipc_status_t::success_k)
            return "a batch does not read";
        // Every field opens with a validity buffer; fixed-size binary takes data, a string offsets
        // and data, in the order of the fields.
        std::array<std::size_t, ipc_fields_max_k> first_buffer {};
        for (std::size_t index = 0, buffer = 0; index != schema.fields.size(); ++index) {
            first_buffer[index] = buffer;
            buffer += schema.fields[index].type == ipc_type_t::fixed_size_binary_k ? 2 : 3;
        }
        std::byte const* const keys = batch.buffers[first_buffer[key_field] + 1].data();
        auto const* const offsets = reinterpret_cast<std::int32_t const*>(
            batch.buffers[first_buffer[smiles_field] + 1].data());
        auto const* const smiles = reinterpret_cast<char const*>(batch.buffers[first_buffer[smiles_field] + 2].data());
        std::byte const* const prints = batch.buffers[first_buffer[fingerprints_field] + 1].data();
        std::size_t const width = static_cast<std::size_t>(schema.fields[fingerprints_field].width);
        for (std::int64_t row = 0; row != batch.length && into.keys.size() < rows; ++row) {
            ustore_key_t key;
            std::memcpy(&key, keys + row * sizeof(key), sizeof(key));
            if (st::failed(into.keys.push_back(ustore_key_t {key})) ||
                st::failed(into.smiles.push_back(std::string_view {
                    smiles + offsets[row], static_cast<std::size_t>(offsets[row + 1] - offsets[row])})) ||
                st::failed(into.fingerprints.push_back(std::span<std::byte const> {prints + row * width, width})))
                return "no room for the rows";
        }
    }
    return into.keys.size() == rows ? nullptr : "the input holds fewer rows than asked";
}

#pragma endregion Input

#pragma region Drives

/** Sectors written to every block device of @p devices, as /proc/diskstats counts them. */
[[nodiscard]] std::uint64_t sectors_written(texts_t const& devices) {
    std::FILE* const stats = std::fopen("/proc/diskstats", "r");
    if (!stats) return 0;
    std::uint64_t total = 0;
    char line[512];
    while (std::fgets(line, sizeof(line), stats)) {
        unsigned major = 0, minor = 0;
        char name[64] {};
        unsigned long long reads = 0, reads_merged = 0, sectors_read = 0, reading = 0, writes = 0, writes_merged = 0,
                           sectors = 0;
        if (std::sscanf(line, "%u %u %63s %llu %llu %llu %llu %llu %llu %llu", &major, &minor, name, &reads,
                        &reads_merged, &sectors_read, &reading, &writes, &writes_merged, &sectors) != 10)
            continue;
        for (std::size_t index = 0; index != devices.size; ++index)
            if (devices.values[index] == std::string_view {name}) total += sectors;
    }
    std::fclose(stats);
    return total;
}

/** Requests in flight on every block device of @p devices, as /proc/diskstats counts them. */
[[nodiscard]] std::uint64_t in_flight(texts_t const& devices) {
    std::FILE* const stats = std::fopen("/proc/diskstats", "r");
    if (!stats) return 0;
    std::uint64_t total = 0;
    char line[512];
    while (std::fgets(line, sizeof(line), stats)) {
        unsigned major = 0, minor = 0;
        char name[64] {};
        unsigned long long fields[9] {};
        if (std::sscanf(line, "%u %u %63s %llu %llu %llu %llu %llu %llu %llu %llu %llu", &major, &minor, name,
                        &fields[0], &fields[1], &fields[2], &fields[3], &fields[4], &fields[5], &fields[6], &fields[7],
                        &fields[8]) != 12)
            continue;
        for (std::size_t index = 0; index != devices.size; ++index)
            if (devices.values[index] == std::string_view {name}) total += fields[8];
    }
    std::fclose(stats);
    return total;
}

/** Bytes in use on the file systems holding @p directories, each counted once. */
[[nodiscard]] std::uint64_t used_bytes(texts_t const& directories) {
    std::uint64_t total = 0;
    st::vector<unsigned long> seen;
    for (std::size_t index = 0; index != directories.size; ++index) {
        std::string const path {directories.values[index]};
        struct statvfs status {};
        if (::statvfs(path.c_str(), &status) != 0) continue;
        bool counted = false;
        for (unsigned long id : seen) counted |= id == status.f_fsid;
        if (counted || st::failed(seen.push_back(static_cast<unsigned long>(status.f_fsid)))) continue;
        total += static_cast<std::uint64_t>(status.f_blocks - status.f_bfree) * status.f_frsize;
    }
    return total;
}

/** Counts the run files created in the data directories, by flushes and merges alike, and the
 *  manifests renamed into them, one per publication. */
struct file_events_t {
    int descriptor = -1;
    std::uint64_t created = 0, renamed = 0;

    explicit file_events_t(texts_t const& directories) noexcept : descriptor(::inotify_init1(IN_NONBLOCK)) {
        for (std::size_t index = 0; descriptor >= 0 && index != directories.size; ++index) {
            std::string const path {directories.values[index]};
            ::inotify_add_watch(descriptor, path.c_str(), IN_CREATE | IN_MOVED_TO);
        }
    }
    ~file_events_t() noexcept {
        if (descriptor >= 0) ::close(descriptor);
    }

    /** Counts every event queued so far. */
    void drain() noexcept {
        alignas(inotify_event) char buffer[64 * 1024];
        for (;;) {
            ssize_t const read = ::read(descriptor, buffer, sizeof(buffer));
            if (read <= 0) return;
            for (char const* at = buffer; at < buffer + read;) {
                auto const* const event = reinterpret_cast<inotify_event const*>(at);
                std::string_view const name {event->len ? event->name : ""};
                if (!(event->mask & IN_ISDIR) && (event->mask & IN_CREATE) && name.ends_with(".ustore")) ++created;
                if (!(event->mask & IN_ISDIR) && (event->mask & IN_MOVED_TO) && name.ends_with(".json")) ++renamed;
                at += sizeof(inotify_event) + event->len;
            }
        }
    }
};

/** The resident bytes of this process, as /proc/self/status counts them. */
[[nodiscard]] std::uint64_t resident_bytes() {
    std::FILE* const status = std::fopen("/proc/self/status", "r");
    if (!status) return 0;
    std::uint64_t kilobytes = 0;
    char line[256];
    while (std::fgets(line, sizeof(line), status))
        if (std::sscanf(line, "VmRSS: %llu kB", reinterpret_cast<unsigned long long*>(&kilobytes)) == 1) break;
    std::fclose(status);
    return kilobytes * 1024;
}

#pragma endregion Drives

#pragma region Database

/** Calls @p call until @p outcome stops answering @c ustore_status_capacity_exhausted_k, as a
 *  thread outside the executor must while every shared channel is out, counting @p retries. */
template <typename call_type_>
void retried(std::atomic<std::uint64_t>& retries, call_type_&& call, ustore_outcome_t const& outcome) noexcept {
    for (call(); outcome.status == ustore_status_capacity_exhausted_k; call()) {
        retries.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::yield();
    }
}

/** Opens the NVMe database @p settings name, answering a reason when it cannot. */
[[nodiscard]] char const* open_database(settings_t const& settings, ustore_database_t& database) {
    std::array<ustore_data_directory_t, texts_t::capacity_k> directories {};
    for (std::size_t index = 0; index != settings.directories.size; ++index) {
        std::string_view const path = settings.directories.values[index];
        directories[index].path = {reinterpret_cast<ustore_byte_t const*>(path.data()), path.size()};
    }
    ustore_outcome_t outcome {};
    ustore_database_open_nvme_t open {
        .database = &database,
        .outcome = &outcome,
        .directory = {reinterpret_cast<ustore_byte_t const*>(settings.open.data()), settings.open.size()},
        .data_directories = settings.directories.size ? directories.data() : nullptr,
        .data_directories_count = settings.directories.size};
    ustore_database_open_nvme(&open);
    return outcome.status == ustore_status_success_k ? nullptr : outcome.message;
}

/** Creates the collection named @p name of @p modality, answering a reason when it cannot. */
[[nodiscard]] char const* create_collection(ustore_database_t database, std::string_view name,
                                            ustore_modality_t modality, ustore_collection_id_t& collection) {
    ustore_outcome_t outcome {};
    ustore_bytes_view_t const spelled {reinterpret_cast<ustore_byte_t const*>(name.data()), name.size()};
    ustore_collection_create_t create {.database = database,
                                       .outcome = &outcome,
                                       .name = spelled,
                                       .modality = modality,
                                       .collection = &collection};
    ustore_collection_create(&create);
    return outcome.status == ustore_status_success_k ? nullptr : outcome.message;
}

#pragma endregion Database

} // namespace unum::ustore::bench

int main(int argc, char const* argv[]) {
    using namespace unum::ustore;
    using namespace unum::ustore::bench;

    settings_t settings;
    std::array<flag_t, 7> const flags {{
        {"--input", "path", "the Arrow IPC stream of rows", &settings.input},
        {"--open", "path", "what the NVMe open takes as its directory", &settings.open},
        {"--directories", "paths", "data directories every run may land on", &settings.directories},
        {"--devices", "names", "block devices the ingest is charged, like nvme5n1", &settings.devices},
        {"--rows", "count", "rows ingested from the front of the input", &settings.rows},
        {"--batch-size", "count", "rows per write call", &settings.batch_size},
        {"--threads", "count", "threads ingesting and reading", &settings.threads},
    }};
    read_settings(argc, argv, settings, flags);
    if (settings.input.empty() || settings.open.empty()) {
        fmt::println(stderr, "Both --input and --open are required");
        return 1;
    }

    molecules_t molecules;
    std::size_t const rows = static_cast<std::size_t>(settings.rows.value);
    if (char const* const failed = load_molecules(settings.input, rows, molecules)) {
        fmt::println(stderr, "Failed to load the molecules: {}", failed);
        return 1;
    }
    std::uint64_t const ingested_bytes = molecules.bytes(rows);
    fmt::println("- loaded {} rows, {:.2f} GB of keys, SMILES and fingerprints", rows,
                 static_cast<double>(ingested_bytes) / 1e9);

    auto maybe_executor = executor_t::make(static_cast<std::size_t>(settings.threads.value),
                                           caller_exclusivity_t::caller_inclusive_k);
    if (!maybe_executor) {
        fmt::println(stderr, "Failed to spawn {} threads", settings.threads.value);
        return 1;
    }
    executor_t& executor = *maybe_executor;

    // Every thread writes its share of batches into both collections; a sampler tracks the space.
    ustore_database_t database = nullptr;
    if (char const* const failed = open_database(settings, database)) {
        fmt::println(stderr, "Failed to open the database: {}", failed);
        return 1;
    }
    ustore_collection_id_t blobs {}, strings {};
    if (char const* const failed = create_collection(database, "molecules", ustore_modality_blobs_k, blobs))
        return fmt::println(stderr, "Failed to create the blobs: {}", failed), 1;
    if (char const* const failed = create_collection(database, "smiles", ustore_modality_strings_k, strings))
        return fmt::println(stderr, "Failed to create the strings: {}", failed), 1;

    // The directory the open names, or the one holding its manifest, is a data directory too.
    texts_t watched = settings.directories;
    std::string const opened_directory =
        std::filesystem::is_directory(std::string {settings.open})
            ? std::string {settings.open}
            : std::filesystem::path {std::string {settings.open}}.parent_path().string();
    if (watched.size != texts_t::capacity_k) watched.values[watched.size++] = opened_directory;
    std::uint64_t const used_before = used_bytes(watched);
    std::uint64_t const sectors_before = sectors_written(settings.devices);
    std::atomic<std::uint64_t> used_peak {used_before};
    file_events_t events {watched};
    std::atomic<bool> ingesting {true};
    std::thread sampler([&] {
        while (ingesting.load(std::memory_order_relaxed)) {
            std::uint64_t const used = used_bytes(watched);
            if (used > used_peak.load(std::memory_order_relaxed)) used_peak.store(used, std::memory_order_relaxed);
            events.drain();
            std::this_thread::sleep_for(std::chrono::milliseconds {200});
        }
    });

    std::atomic<std::uint64_t> retries {0};
    std::size_t const batch = static_cast<std::size_t>(settings.batch_size.value);
    std::size_t const batches = divide_round_up(rows, batch);
    time_point_t const ingest_started = steady_clock_t::now();
    row_t const ingest = run_pass(executor, "pubchem_ingest", batches, [&](loop_t& loop, std::size_t) noexcept {
        st::vector<std::uint32_t> offsets;
        st::vector<std::uint8_t> tape;
        st::vector<std::uint32_t> smiles_offsets;
        st::vector<std::uint8_t> smiles_tape;
        ustore_outcome_t outcome {};
        for (std::size_t call : loop) {
            std::size_t const first = call * batch;
            std::size_t const count = fu::min_of(batch, rows - first);
            if (st::failed(offsets.resize(count + 1)) || st::failed(smiles_offsets.resize(count + 1))) {
                loop.fail();
                continue;
            }
            std::size_t print_bytes = 0, smiles_bytes = 0;
            for (std::size_t row = 0; row != count; ++row) {
                offsets[row] = static_cast<std::uint32_t>(print_bytes);
                smiles_offsets[row] = static_cast<std::uint32_t>(smiles_bytes);
                print_bytes += molecules.fingerprints[first + row].size();
                smiles_bytes += molecules.smiles[first + row].size();
            }
            offsets[count] = static_cast<std::uint32_t>(print_bytes);
            smiles_offsets[count] = static_cast<std::uint32_t>(smiles_bytes);
            if (st::failed(tape.resize(print_bytes)) || st::failed(smiles_tape.resize(smiles_bytes))) {
                loop.fail();
                continue;
            }
            for (std::size_t row = 0; row != count; ++row) {
                std::memcpy(tape.data() + offsets[row], molecules.fingerprints[first + row].data(),
                            molecules.fingerprints[first + row].size());
                std::memcpy(smiles_tape.data() + smiles_offsets[row], molecules.smiles[first + row].data(),
                            molecules.smiles[first + row].size());
            }
            ustore_blobs_write_t write {
                .database = database,
                .outcome = &outcome,
                .count = count,
                .collections = {&blobs, 0},
                .keys = {molecules.keys.data() + first, sizeof(ustore_key_t)},
                .values = {.layout = ustore_strings_u32_tape_k, .data = {.u32_tape = {tape.data(), offsets.data()}}}};
            retried(retries, [&] { ustore_blobs_write(&write); }, outcome);
            if (outcome.status != ustore_status_success_k) {
                loop.fail();
                continue;
            }
            ustore_strings_write_t map {.database = database,
                                        .outcome = &outcome,
                                        .count = count,
                                        .collections = {&strings, 0},
                                        .strings = {.layout = ustore_strings_u32_tape_k,
                                                    .data = {.u32_tape = {smiles_tape.data(), smiles_offsets.data()}}},
                                        .values = {molecules.keys.data() + first, sizeof(ustore_key_t)}};
            retried(retries, [&] { ustore_strings_write(&map); }, outcome);
            if (outcome.status != ustore_status_success_k) {
                loop.fail();
                continue;
            }
            loop.count(count, print_bytes + smiles_bytes + 2 * count * sizeof(ustore_key_t));
        }
    });
    print(ingest);
    fmt::println("- pubchem_ingest: {} calls retried while every shared channel was out", retries.exchange(0));

    // Closing flushes the memory runs, which the ingest is charged as well.
    ustore_database_free(database);
    database = nullptr;
    double const ingest_seconds = std::chrono::duration<double>(steady_clock_t::now() - ingest_started).count();
    ingesting.store(false, std::memory_order_relaxed);
    sampler.join();
    events.drain();
    std::uint64_t const sectors_after = sectors_written(settings.devices);
    std::uint64_t const used_after = used_bytes(watched);
    double const written_bytes = static_cast<double>(sectors_after - sectors_before) * 512.0;

    // The mount: what reopening the database costs in time and in resident bytes.
    std::uint64_t const resident_before = resident_bytes();
    time_point_t const mount_started = steady_clock_t::now();
    if (char const* const failed = open_database(settings, database)) {
        fmt::println(stderr, "Failed to reopen the database: {}", failed);
        return 1;
    }
    double const mount_seconds = std::chrono::duration<double>(steady_clock_t::now() - mount_started).count();
    std::uint64_t const resident_after = resident_bytes();
    ustore_outcome_t outcome {};
    ustore_database_description_t description {};
    ustore_database_describe_t describe {.database = database, .outcome = &outcome, .description = &description};
    ustore_database_describe(&describe);

    // Point reads of random keys of the ingest, while a sampler reads the depth the drives see.
    auto blobs_reads = [&](executor_t& readers, std::string const& name, std::size_t batch) {
        if (!settings.selects(name)) return;
        std::atomic<bool> reading {true}, reported {false};
        std::uint64_t depth_sum = 0, depth_samples = 0, depth_peak = 0;
        std::thread depths([&] {
            while (reading.load(std::memory_order_relaxed)) {
                std::uint64_t const depth = in_flight(settings.devices);
                depth_sum += depth, ++depth_samples, depth_peak = fu::max_of(depth_peak, depth);
                std::this_thread::sleep_for(std::chrono::milliseconds {1});
            }
        });
        row_t const row = run_threads(readers, settings, name, [&](loop_t& loop, std::size_t thread) noexcept {
            ustore_arena_t arena = nullptr;
            ustore_outcome_t local {};
            ustore_arena_init_t init {.arena = &arena, .outcome = &local};
            ustore_arena_init(&init);
            st::vector<ustore_key_t> keys;
            if (st::failed(keys.resize(batch))) return loop.skip("no room for keys");
            splitmix64_t random {stream_key(settings.seed.value, name, thread)};
            for ([[maybe_unused]] std::size_t call : loop) {
                for (ustore_key_t& key : keys) key = molecules.keys[random.next() % rows];
                ustore_strings_view_t values {};
                ustore_blobs_read_t read {.database = database,
                                          .outcome = &local,
                                          .count = batch,
                                          .collections = {&blobs, 0},
                                          .keys = {keys.data(), sizeof(ustore_key_t)},
                                          .values = &values,
                                          .arena = arena};
                retried(retries, [&] { ustore_blobs_read(&read); }, local);
                if (local.status == ustore_status_success_k) loop.count(batch);
                else if (loop.fail(), !reported.exchange(true)) fmt::println(stderr, "- {}: {}", name, local.message);
            }
            ustore_arena_free(arena);
        });
        reading.store(false, std::memory_order_relaxed);
        depths.join();
        print(row);
        fmt::println("- {}: {:.1f} reads in flight on average, {} at the peak, {} calls retried", name,
                     depth_samples ? static_cast<double>(depth_sum) / static_cast<double>(depth_samples) : 0.0,
                     depth_peak, retries.exchange(0));
    };
    for (std::size_t const batch : {std::size_t {32}, std::size_t {1024}})
        blobs_reads(executor, fmt::format("pubchem_blobs_read_batch{}", batch), batch);
    auto maybe_readers = executor_t::make(32, caller_exclusivity_t::caller_inclusive_k);
    if (maybe_readers) blobs_reads(*maybe_readers, "pubchem_blobs_read_single_32_threads", 1);
    for (std::size_t const depth : {std::size_t {32}, std::size_t {1024}}) {
        std::string const strings_name = fmt::format("pubchem_strings_read_batch{}", depth);
        if (settings.selects(strings_name))
            print(run_threads(executor, settings, strings_name, [&](loop_t& loop, std::size_t thread) noexcept {
                ustore_arena_t arena = nullptr;
                ustore_outcome_t local {};
                ustore_arena_init_t init {.arena = &arena, .outcome = &local};
                ustore_arena_init(&init);
                st::vector<std::uint32_t> offsets;
                st::vector<std::uint8_t> tape;
                if (st::failed(offsets.resize(depth + 1))) return loop.skip("no room for strings");
                splitmix64_t random {stream_key(settings.seed.value, strings_name, thread)};
                for ([[maybe_unused]] std::size_t call : loop) {
                    tape.clear();
                    for (std::size_t index = 0; index != depth; ++index) {
                        std::string_view const smiles = molecules.smiles[random.next() % rows];
                        offsets[index] = static_cast<std::uint32_t>(tape.size());
                        std::size_t const at = tape.size();
                        if (st::failed(tape.resize(at + smiles.size()))) return loop.skip("no room for strings");
                        std::memcpy(tape.data() + at, smiles.data(), smiles.size());
                    }
                    offsets[depth] = static_cast<std::uint32_t>(tape.size());
                    ustore_key_t const* found = nullptr;
                    ustore_strings_read_t read {.database = database,
                                                .outcome = &local,
                                                .count = depth,
                                                .collections = {&strings, 0},
                                                .strings = {.layout = ustore_strings_u32_tape_k,
                                                            .data = {.u32_tape = {tape.data(), offsets.data()}}},
                                                .values = &found,
                                                .arena = arena};
                    retried(retries, [&] { ustore_strings_read(&read); }, local);
                    if (local.status != ustore_status_success_k) loop.fail();
                    else loop.count(depth);
                }
                ustore_arena_free(arena);
            }));
    }
    ustore_database_free(database);

    double const gigabytes = static_cast<double>(ingested_bytes) / 1e9;
    fmt::println("- ingest: {:.2f} M rows/s, {:.1f} MB/s over {:.1f} s",
                 static_cast<double>(rows) / ingest_seconds / 1e6,
                 static_cast<double>(ingested_bytes) / ingest_seconds / 1e6, ingest_seconds);
    fmt::println("- write amplification: {:.2f} bytes written per ingested byte, {:.2f} GB written",
                 written_bytes / static_cast<double>(ingested_bytes), written_bytes / 1e9);
    fmt::println("- space: peak {:.2f} GB above the start, {:.2f} GB at the end, extra at peak {:.2f} GB",
                 static_cast<double>(used_peak.load() - used_before) / 1e9,
                 static_cast<double>(used_after - used_before) / 1e9,
                 (static_cast<double>(used_peak.load() - used_before) - static_cast<double>(ingested_bytes)) / 1e9);
    fmt::println(
        "- mount: {:.3f} s, {:.1f} resident bytes per key, engine holds {:.1f} bytes per key of {} keys", mount_seconds,
        description.keys ? static_cast<double>(resident_after - resident_before) / static_cast<double>(description.keys)
                         : 0.0,
        description.keys ? static_cast<double>(description.memory_bytes) / static_cast<double>(description.keys) : 0.0,
        description.keys);
    fmt::println("- files: {:.1f} runs written per GB, {:.1f} manifests replaced per GB",
                 static_cast<double>(events.created) / gigabytes, static_cast<double>(events.renamed) / gigabytes);
    fmt::println("- ingested {:.2f} GB", gigabytes);
    return 0;
}
