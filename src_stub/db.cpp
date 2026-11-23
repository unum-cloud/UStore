/**
 *  @file src_stub/db.cpp
 *  @author Ash Vardanian
 *  @date November 16, 2026
 *  @brief Databases, their image, snapshots, collections, transactions, change feeds, ingests and
 *      the compaction plan, over the store of @c lib.hpp.
 *
 *  Only the in-memory engine serves: RocksDB, LevelDB and NVMe opens check their arguments as those
 *  engines do and then refuse as a build without them does. The RAM image is the engine's format
 *  byte for byte, so a directory one library wrote the other opens, apart from pages, whose stored
 *  form is the stand-in's own.
 */
#include <cerrno> // `errno`, `ENOENT` - a directory without an image
#include <cstdio> // `std::rename`, `std::remove`

#include "lib.hpp"

using namespace unum::ustore::stub;

namespace unum::ustore::stub {

bool refuse_cpu_class_ceiling(call_t& call, ustore_cpu_class_t cpu_class_ceiling) noexcept;

/** Leads every image, as the RAM engine's does, so a file of another format is refused
 *  rather than misread. */
constexpr char image_signature_k[8] = {'U', 'S', 'T', 'S', 'M', 'T', 'B', '1'};

/** Name of the image inside the database directory. */
constexpr std::string_view image_name_k = "/ram.image";

template <typename value_type_>
void append_value(std::string& image, value_type_ const& value) {
    image.append(reinterpret_cast<char const*>(&value), sizeof(value));
}

inline void append_blob(std::string& image, std::string_view blob) {
    append_value(image, static_cast<std::uint64_t>(blob.size()));
    image.append(blob);
}

/** Serializes every collection @p database holds at @p stamp, recording the bytes of the keys
 *  and values of each in @p sizes, under the lock the caller holds. */
void build_image(ustore_database_handle_t const& database, stamp_t stamp, std::string& image,
                 std::vector<std::size_t>& sizes) {
    image.assign(image_signature_k, sizeof(image_signature_k));
    sizes.clear();
    reader_t reader;
    reader.database = const_cast<ustore_database_handle_t*>(&database);
    reader.stamp = stamp;
    for (auto const& [id, collection] : database.collections) {
        if (collection.created > stamp) continue;
        image.append(id);
        append_value(image, static_cast<std::uint8_t>(collection.modality));
        append_blob(image, collection.name);
        std::size_t const count_offset = image.size();
        append_value(image, std::uint64_t {0});
        std::uint64_t count = 0;
        std::size_t bytes = 0;
        for (auto const& [key, history] : collection.entries) {
            version_t const* const version = visible_version(history, stamp);
            if (!version || !version->present) continue;
            ++count;
            bytes += key.size() + version->value.size();
            switch (collection.modality) {
            case ustore_modality_graph_k: image.append(key); break;
            case ustore_modality_strings_k:
                append_blob(image, key);
                image.append(version->value.data(), 16);
                break;
            default:
                image.append(key);
                append_blob(image, version->value);
                break;
            }
        }
        std::memcpy(image.data() + count_offset, &count, sizeof(count));
        sizes.push_back(bytes);
    }
}

/** Writes @p image into @p directory beside its previous one and renames it over, reporting the
 *  bytes of every collection's keys and values to @p progress; a cancelled or failed write
 *  leaves the previous image whole. */
status_t store_image(std::string const& directory, std::string const& image, std::vector<std::size_t> const& sizes,
                     ustore_progress_callback_t progress, void* progress_state) {
    std::string const path = directory + std::string(image_name_k);
    std::string const staging = path + ".staging";
    std::FILE* const file = std::fopen(staging.c_str(), "wb");
    if (!file) return {ustore_status_io_k, "Failed to open the image"};
    bool const written = std::fwrite(image.data(), 1, image.size(), file) == image.size();
    bool const closed = std::fclose(file) == 0;
    if (!written || !closed) {
        std::remove(staging.c_str());
        return {ustore_status_io_k, "Failed to write the image"};
    }
    std::size_t total = 0, finished = 0;
    for (std::size_t bytes : sizes) total += bytes;
    for (std::size_t bytes : sizes) {
        finished += bytes;
        if (!carries_on(progress, progress_state, finished, total)) {
            std::remove(staging.c_str());
            return {ustore_status_cancelled_k, "The progress callback cancelled the save"};
        }
    }
    if (std::rename(staging.c_str(), path.c_str()) != 0) {
        std::remove(staging.c_str());
        return {ustore_status_io_k, "Failed to replace the image"};
    }
    return {};
}

/** Takes @p bytes from the front of @p image into @p into, answering false where it ends. */
bool take_bytes(std::string_view& image, void* into, std::size_t bytes) noexcept {
    if (image.size() < bytes) return false;
    if (bytes) std::memcpy(into, image.data(), bytes);
    image.remove_prefix(bytes);
    return true;
}

bool take_blob(std::string_view& image, std::string& blob) {
    std::uint64_t length = 0;
    if (!take_bytes(image, &length, sizeof(length)) || length > image.size()) return false;
    blob.assign(image.data(), static_cast<std::size_t>(length));
    image.remove_prefix(static_cast<std::size_t>(length));
    return true;
}

/** Takes one entry of a collection of @p modality from the front of @p image, answering false where
 *  it ends. */
bool take_entry(std::string_view& image, ustore_modality_t modality, std::string& key, std::string& value) {
    switch (modality) {
    case ustore_modality_graph_k:
        if (image.size() < half_edge_bytes_k) return false;
        key.assign(image.data(), half_edge_bytes_k);
        image.remove_prefix(half_edge_bytes_k);
        return true;
    case ustore_modality_strings_k:
        if (!take_blob(image, key) || image.size() < 16) return false;
        value.assign(image.data(), 16);
        image.remove_prefix(16);
        return true;
    default:
        if (image.size() < 16) return false;
        key.assign(image.data(), 16);
        image.remove_prefix(16);
        return take_blob(image, value);
    }
}

/** Loads the image of @p directory into @p database, leaving it empty when there is none. */
status_t read_image(ustore_database_handle_t& database, std::string const& directory) {
    std::string const path = directory + std::string(image_name_k);
    std::FILE* const file = std::fopen(path.c_str(), "rb");
    if (!file && errno == ENOENT) return {};
    if (!file) return {ustore_status_io_k, "Failed to open the image"};
    std::string bytes;
    char buffer[1 << 16];
    for (std::size_t read; (read = std::fread(buffer, 1, sizeof(buffer), file)) != 0;) bytes.append(buffer, read);
    bool const failed = std::ferror(file);
    std::fclose(file);
    if (failed) return {ustore_status_io_k, "Failed to read the image"};
    std::string_view image = bytes;
    char signature[sizeof(image_signature_k)];
    if (!take_bytes(image, signature, sizeof(signature)) ||
        std::memcmp(signature, image_signature_k, sizeof(signature)) != 0)
        return {ustore_status_malformed_k, "The image has an unknown format"};
    status_t const truncated {ustore_status_malformed_k, "The image is truncated"};
    while (!image.empty()) {
        collection_t collection;
        std::uint8_t modality = 0;
        std::uint64_t count = 0;
        if (!take_bytes(image, collection.id.uuid.bytes, 16)) return truncated;
        if (!take_bytes(image, &modality, 1)) return truncated;
        if (modality < ustore_modality_blobs_k || modality > ustore_modality_pages_k)
            return {ustore_status_malformed_k, "The image names an unknown modality"};
        collection.modality = static_cast<ustore_modality_t>(modality);
        if (!take_blob(image, collection.name) || !take_bytes(image, &count, sizeof(count))) return truncated;
        for (std::uint64_t index = 0; index != count; ++index) {
            std::string key, value;
            if (!take_entry(image, collection.modality, key, value)) return truncated;
            collection.entries[std::move(key)].push_back({0, true, std::move(value)});
        }
        database.collections[bytes_of(collection.id)] = std::move(collection);
    }
    return {};
}

/** Refuses an isolation no constant names. */
bool refuse_isolation(call_t& call, ustore_isolation_t isolation) noexcept {
    auto const bits = std::bit_cast<std::underlying_type_t<ustore_isolation_t>>(isolation);
    if (bits != ustore_isolation_strict_serializable_k && bits != ustore_isolation_snapshot_k)
        return call.stop(ustore_status_invalid_argument_k, "Unknown isolation");
    return false;
}

/** Refuses a directory carrying a length without bytes, or a NUL byte no path may hold. */
bool refuse_path(call_t& call, ustore_bytes_view_t path) noexcept {
    if (path.length && !path.data)
        return call.stop(ustore_status_invalid_argument_k, "A directory with a length needs its bytes");
    if (holds_nul(text_of(path))) return call.stop(ustore_status_invalid_argument_k, "A path never holds a NUL byte");
    return false;
}

/** Refuses counted data directories that are null, or one with an empty path or a NUL byte. */
bool refuse_data_directories(call_t& call, ustore_data_directory_t const* directories, ustore_size_t count) noexcept {
    if (count && !directories)
        return call.stop(ustore_status_invalid_argument_k, "The counted data directories are null");
    for (std::size_t index = 0; index != count; ++index) {
        if (!directories[index].path.length)
            return call.stop(ustore_status_invalid_argument_k, "A data directory has an empty path");
        if (refuse_path(call, directories[index].path)) return true;
    }
    return false;
}

/** Makes a database for the RAM engine into @p made. */
ustore_database_handle_t* make_database(call_t& call, ustore_isolation_t isolation) {
    auto* const database = new (std::nothrow) ustore_database_handle_t {};
    if (!database) call.refuse(ustore_status_out_of_memory_k, "Failed to allocate the database");
    database->isolation = isolation;
    return database;
}

/** Whether an open lists further data directories, as RocksDB and NVMe opens do. */
template <typename arguments_type_>
concept lists_data_directories_concept = requires(arguments_type_ const& arguments) { arguments.data_directories; };

/** Whether an open caps the CPU class, which only the NVMe open among the missing engines takes. */
template <typename arguments_type_>
concept caps_cpu_class_concept = requires(arguments_type_ const& arguments) { arguments.cpu_class_ceiling; };

/** Whether a transaction call asks for a durability, as a commit does. */
template <typename arguments_type_>
concept asks_durability_concept = requires(arguments_type_ const& arguments) { arguments.durability; };

/** Whether a collection of @p database carries @p name; an empty name is never taken. */
bool name_taken(ustore_database_handle_t const& database, std::string_view name) noexcept {
    if (name.empty()) return false;
    for (auto const& [id, collection] : database.collections)
        if (collection.name == name) return true;
    return false;
}

/** Answers every open of an engine the stand-in does not carry, once its arguments passed as
 *  that engine checks. */
template <typename arguments_type_>
void refuse_engine(arguments_type_* arguments, char const* message) {
    call_t call(arguments->outcome);
    if (arguments->database) *arguments->database = nullptr;
    if (!arguments->database)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the database");
    if (!arguments->directory.length) return call.refuse(ustore_status_invalid_argument_k, "The directory is empty");
    if (refuse_path(call, arguments->directory)) return;
    if constexpr (lists_data_directories_concept<arguments_type_>)
        if (refuse_data_directories(call, arguments->data_directories, arguments->data_directories_count)) return;
    if constexpr (caps_cpu_class_concept<arguments_type_>) {
        if (refuse_cpu_class_ceiling(call, arguments->cpu_class_ceiling)) return;
        std::string_view const manifest = text_of(arguments->directory);
        if (!manifest.ends_with(".ustore.json") && !manifest.starts_with("/dev/"))
            return call.refuse(ustore_status_invalid_argument_k,
                               "The directory names no .ustore.json manifest nor raw address");
    }
    call.refuse(ustore_status_unsupported_k, message);
}

/** Refuses a drop mode no constant names. */
bool refuse_drop_mode(call_t& call, ustore_drop_mode_t mode) noexcept {
    auto const bits = std::bit_cast<std::underlying_type_t<ustore_drop_mode_t>>(mode);
    if (bits != ustore_drop_collection_k && bits != ustore_drop_pairs_k && bits != ustore_drop_values_k)
        return call.stop(ustore_status_invalid_argument_k, "Unknown drop mode");
    return false;
}

/** Stages into @p batch the removal of every key @p collection holds at the head, or the
 *  emptying of every value. */
void stage_emptying(collection_t const& collection, ustore_drop_mode_t mode, batch_t& batch) {
    overlay_t& overlay = batch[bytes_of(collection.id)];
    for (auto const& [key, history] : collection.entries) {
        if (history.empty() || !history.back().present) continue;
        if (mode == ustore_drop_pairs_k) overlay[key] = {false, {}, ustore_change_removal_k, true};
        else overlay[key] = {true, {}, ustore_change_write_k, true};
    }
}

/** The RAM engine's phases in order: a merge per collection, then the reclaim. */
constexpr ustore_phase_t compact_phases_k[] = {ustore_phase_merge_k, ustore_phase_reclaim_k};

/** The position of @p phase among the ones the plan runs, from zero, the end counting
 *  past the last. */
std::size_t ordinal_of(ustore_phase_t phase) noexcept {
    for (std::size_t index = 0; index != std::size(compact_phases_k); ++index)
        if (compact_phases_k[index] == phase) return index;
    return phase == ustore_phase_done_k ? std::size(compact_phases_k) : 0;
}

/** Merges one collection: drops every version no pinned reader reaches, and every key whose
 *  newest one is a removal none of them sees. */
void merge_collection(ustore_database_handle_t& database, std::string const& id) {
    std::unique_lock lock(database.mutex);
    auto found = database.collections.find(id);
    if (found == database.collections.end()) return;
    stamp_t const oldest = oldest_pin(database);
    auto& entries = found->second.entries;
    for (auto it = entries.begin(); it != entries.end();) {
        prune(it->second, oldest);
        bool const gone = it->second.size() == 1 && !it->second.front().present && it->second.front().stamp <= oldest;
        it = gone ? entries.erase(it) : std::next(it);
    }
    stamp_t const oldest_feed = database.feed_pins.empty() ? database.head : *database.feed_pins.begin();
    auto& changes = found->second.changes;
    changes.erase(std::remove_if(changes.begin(), changes.end(),
                                 [&](change_t const& change) { return change.stamp <= oldest_feed; }),
                  changes.end());
}

/** Reclaims: rewrites the image that names what the merges kept. */
status_t reclaim(ustore_database_handle_t& database) {
    if (database.directory.empty()) return {};
    std::unique_lock lock(database.mutex);
    return write_image(database, database.head, database.directory);
}

/** Units of @p plan in all: one merge per collection it listed, then the reclaim. */
std::size_t units_of(ustore_database_compact_plan_handle_t const& plan) noexcept { return plan.collections.size() + 1; }

/** Units of @p plan already done, counted from its open. */
std::size_t units_done(ustore_database_compact_plan_handle_t const& plan) noexcept {
    switch (plan.phase) {
    case ustore_phase_merge_k: return plan.completed;
    case ustore_phase_reclaim_k: return plan.collections.size() + plan.completed;
    case ustore_phase_done_k: return units_of(plan);
    default: return 0;
    }
}

/** Writes where @p plan stands into @p phase. */
void report_phase(ustore_database_compact_plan_handle_t const& plan, ustore_phase_progress_t* phase) noexcept {
    if (!phase) return;
    phase->phase = plan.phase;
    phase->ordinal = ordinal_of(plan.phase);
    phase->completed = plan.completed;
    phase->total = plan.phase == ustore_phase_merge_k     ? plan.collections.size()
                   : plan.phase == ustore_phase_reclaim_k ? 1
                                                          : 0;
}

/** Runs @p plan from the unit it stopped at towards its end, reporting every unit against the
 *  whole plan, until a cancel, which a report after the last unit leaves nothing to stop. */
void advance_plan(call_t& call, ustore_database_compact_plan_handle_t& plan, ustore_progress_callback_t progress,
                  void* progress_state) {
    ustore_database_handle_t& database = *plan.database;
    std::size_t const total = units_of(plan);
    auto report = [&]() noexcept {
        std::size_t const done = units_done(plan);
        if (carries_on(progress, progress_state, done, total) || done == total) return true;
        return call.fail(ustore_status_cancelled_k, "The progress callback cancelled the compaction");
    };
    if (plan.phase == ustore_phase_none_k) plan.phase = ustore_phase_merge_k, plan.completed = 0;
    while (plan.phase == ustore_phase_merge_k && plan.completed != plan.collections.size()) {
        merge_collection(database, plan.collections[plan.completed]);
        ++plan.completed;
        if (!report()) return;
    }
    if (plan.phase == ustore_phase_merge_k) plan.phase = ustore_phase_reclaim_k, plan.completed = 0;
    if (plan.phase == ustore_phase_reclaim_k) {
        if (call.failed_with(reclaim(database))) return;
        plan.phase = ustore_phase_done_k, plan.completed = 0;
        report();
    }
}

/** Lists the collections a plan opening on @p database compacts, at the head. */
void list_plan_collections(ustore_database_handle_t& database, ustore_database_compact_plan_handle_t& plan) {
    std::shared_lock lock(database.mutex);
    for (auto const& [id, collection] : database.collections) plan.collections.push_back(id);
}

/** The checks stage, commit and rollback share: a null handle, a transaction of another
 *  database, then the claims. */
template <typename arguments_type_>
bool open_transaction_call(call_t& call, arguments_type_ const& arguments) {
    if (refuse_database(call, arguments.database)) return false;
    if (!arguments.transaction) return call.fail(ustore_status_uninitialized_k, "The transaction is not open");
    call.name(&arguments.transaction->named_calls);
    if constexpr (asks_durability_concept<arguments_type_>) {
        auto const durability = std::bit_cast<std::underlying_type_t<ustore_durability_t>>(arguments.durability);
        if (durability != ustore_durability_buffered_k && durability != ustore_durability_flushed_k)
            return call.fail(ustore_status_invalid_argument_k, "Unknown durability");
    }
    if (arguments.transaction->magic != magic_t::transaction_k || arguments.transaction->database != arguments.database)
        return call.fail(ustore_status_invalid_argument_k, "The transaction belongs to another database");
    if (refuse_read_only(call, arguments.database)) return false;
    if (!call.hold_executor(arguments.executor)) return false;
    return call.claim(&arguments.transaction->claim, "Another call holds the transaction");
}

status_t write_image(ustore_database_handle_t& database, stamp_t stamp, std::string const& directory,
                     ustore_progress_callback_t progress, void* progress_state) {
    try {
        std::string image;
        std::vector<std::size_t> sizes;
        build_image(database, stamp, image, sizes);
        return store_image(directory, image, sizes, progress, progress_state);
    }
    catch (std::bad_alloc const&) {
        return {ustore_status_out_of_memory_k, "Out of memory while writing the image"};
    }
}

} // namespace unum::ustore::stub

void ustore_database_init_ram(ustore_database_init_ram_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->database) *arguments->database = nullptr;
    if (!arguments->database)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the database");
    if (refuse_isolation(call, arguments->isolation) || refuse_cpu_class_ceiling(call, arguments->cpu_class_ceiling))
        return;
    if (!call.hold_executor(arguments->executor)) return;
    *arguments->database = make_database(call, arguments->isolation);
}

void ustore_database_open_ram(ustore_database_open_ram_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->database) *arguments->database = nullptr;
    if (!arguments->database)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the database");
    if (!arguments->directory.length) return call.refuse(ustore_status_invalid_argument_k, "The directory is empty");
    if (refuse_path(call, arguments->directory) || refuse_isolation(call, arguments->isolation) ||
        refuse_cpu_class_ceiling(call, arguments->cpu_class_ceiling))
        return;
    if (!call.hold_executor(arguments->executor)) return;
    std::string const directory(text_of(arguments->directory));
    if (!directory_exists(directory)) return call.refuse(ustore_status_not_found_k, "No such directory");
    ustore_database_handle_t* const database = make_database(call, arguments->isolation);
    if (!database) return;
    database->directory = directory;
    try {
        if (call.failed_with(read_image(*database, directory))) return delete database;
    }
    catch (std::bad_alloc const&) {
        delete database;
        return call.refuse(ustore_status_out_of_memory_k, "Out of memory while loading the image");
    }
    *arguments->database = database;
}

void ustore_database_open_rocksdb(ustore_database_open_rocksdb_t* arguments) {
    refuse_engine(arguments, "The library was built without RocksDB");
}

void ustore_database_view_rocksdb(ustore_database_view_rocksdb_t* arguments) {
    refuse_engine(arguments, "The library was built without RocksDB");
}

void ustore_database_open_leveldb(ustore_database_open_leveldb_t* arguments) {
    refuse_engine(arguments, "The library was built without LevelDB");
}

void ustore_database_open_nvme(ustore_database_open_nvme_t* arguments) {
    refuse_engine(arguments, "The library was built without the NVMe engine");
}

void ustore_database_describe(ustore_database_describe_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->description) *arguments->description = {};
    if (refuse_database(call, arguments->database)) return;
    if (!arguments->description)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the description");
    if (!call.hold_executor(arguments->executor)) return;
    ustore_database_handle_t& database = *arguments->database;
    ustore_database_description_t description {};
    description.capabilities = static_cast<ustore_capabilities_t>(
        ustore_capabilities_transactions_k | ustore_capabilities_snapshots_k | ustore_capabilities_saves_snapshots_k);
    description.engine = ustore_engine_ram_k;
    description.isolation = database.isolation;
    // Serial is the one class the stand-in carries, so every ceiling picks it
    description.cpu_class = ustore_cpu_class_serial_k;
    std::shared_lock lock(database.mutex);
    for (auto const& [id, collection] : database.collections)
        for (auto const& [key, history] : collection.entries) {
            if (!history.empty() && history.back().present) ++description.keys;
            for (version_t const& version : history) description.memory_bytes += key.size() + version.value.size();
        }
    *arguments->description = description;
}

void ustore_database_flush(ustore_database_flush_t* arguments) {
    call_t call(arguments->outcome);
    if (refuse_database(call, arguments->database) || refuse_read_only(call, arguments->database)) return;
    if (!call.hold_executor(arguments->executor)) return;
    ustore_database_handle_t& database = *arguments->database;
    if (database.directory.empty()) return;
    std::unique_lock lock(database.mutex);
    call.failed_with(write_image(database, database.head, database.directory));
}

void ustore_database_free(ustore_database_t database) {
    if (!database) return;
    wait_named(database->named_calls);
    if (!database->directory.empty()) {
        std::unique_lock lock(database->mutex);
        // A free cannot report, so an image that fails to save leaves the last one saved in place
        [[maybe_unused]] status_t const saved = write_image(*database, database->head, database->directory);
    }
    release_database(database);
}

void ustore_database_clear(ustore_database_clear_t* arguments) {
    call_t call(arguments->outcome);
    if (refuse_database(call, arguments->database)) return;
    if (refuse_drop_mode(call, arguments->mode) || refuse_read_only(call, arguments->database)) return;
    if (!call.hold_executor(arguments->executor)) return;
    ustore_database_handle_t& database = *arguments->database;
    std::unique_lock lock(database.mutex);
    if (arguments->mode == ustore_drop_collection_k) {
        if (database.snapshots_open)
            return call.refuse(ustore_status_invalid_argument_k, "A snapshot is open while the drop runs");
        database.collections.clear();
        ++database.head;
        if (!database.directory.empty()) call.failed_with(write_image(database, database.head, database.directory));
        return;
    }
    try {
        batch_t batch;
        for (auto const& [id, collection] : database.collections)
            if (arguments->mode == ustore_drop_pairs_k || collection.modality != ustore_modality_strings_k)
                stage_emptying(collection, arguments->mode, batch);
        stamp_t stamp = 0;
        call.failed_with(apply_batch(database, batch, stamp));
    }
    catch (std::bad_alloc const&) {
        call.refuse(ustore_status_out_of_memory_k, "Out of memory while clearing");
    }
}

void ustore_database_compact(ustore_database_compact_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->phase) *arguments->phase = {};
    if (refuse_database(call, arguments->database)) return;
    ustore_database_handle_t& database = *arguments->database;
    ustore_database_compact_plan_handle_t* const live = arguments->plan ? *arguments->plan : nullptr;
    if (arguments->plan && !arguments->executor)
        return call.refuse(ustore_status_invalid_argument_k, "A plan opens only with an executor");
    if (live && (live->database != &database || live->executor != arguments->executor))
        return call.refuse(ustore_status_invalid_argument_k, "The plan was opened on another database or executor");
    if (refuse_read_only(call, arguments->database)) return;
    if (!call.hold_executor(arguments->executor)) return;
    if (live) {
        call.name(&live->named_calls);
        if (!call.claim(&live->claim, "Another call advances the plan")) return;
        advance_plan(call, *live, arguments->progress, arguments->progress_state);
        return report_phase(*live, arguments->phase);
    }

    // Opening a plan, for the call alone or for the caller to keep, takes the database's one slot
    ustore_database_compact_plan_handle_t* const plan = new (std::nothrow) ustore_database_compact_plan_handle_t {};
    if (!plan) return call.refuse(ustore_status_out_of_memory_k, "Failed to allocate the plan");
    plan->database = &database;
    plan->executor = arguments->executor;
    {
        std::unique_lock lock(database.mutex);
        if (database.live_plan) {
            delete plan;
            return call.refuse(ustore_status_contended_k, "Another live plan compacts the database");
        }
        database.live_plan = plan;
    }
    try {
        list_plan_collections(database, *plan);
    }
    catch (std::bad_alloc const&) {
        std::unique_lock lock(database.mutex);
        database.live_plan = nullptr;
        delete plan;
        return call.refuse(ustore_status_out_of_memory_k, "Failed to list the collections");
    }
    advance_plan(call, *plan, arguments->progress, arguments->progress_state);
    report_phase(*plan, arguments->phase);
    if (arguments->plan) {
        *arguments->plan = plan;
        return;
    }
    std::unique_lock lock(database.mutex);
    database.live_plan = nullptr;
    delete plan;
}

void ustore_database_compact_plan_finish(ustore_database_compact_plan_finish_t* arguments) {
    call_t call(arguments->outcome);
    ustore_database_compact_plan_handle_t* const plan = arguments->plan;
    // The plan goes whatever the call answers, once no call naming it is in flight
    if (plan) {
        wait_named(plan->named_calls);
        while (plan->claim.held()) std::this_thread::yield();
        {
            std::unique_lock lock(plan->database->mutex);
            if (plan->database->live_plan == plan) plan->database->live_plan = nullptr;
        }
    }
    bool const foreign = plan && (plan->database != arguments->database || plan->executor != arguments->executor);
    delete plan;
    if (refuse_database(call, arguments->database)) return;
    if (!plan) return call.refuse(ustore_status_invalid_argument_k, "The call needs a plan to finish");
    if (foreign)
        return call.refuse(ustore_status_invalid_argument_k, "The plan was opened on another database or executor");
    call.hold_executor(arguments->executor);
}

void ustore_snapshot_init(ustore_snapshot_init_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->snapshot) *arguments->snapshot = nullptr;
    if (arguments->read_stamp) *arguments->read_stamp = 0;
    if (refuse_database(call, arguments->database)) return;
    if (!arguments->snapshot)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the snapshot");
    if (!call.hold_executor(arguments->executor)) return;
    ustore_database_handle_t& database = *arguments->database;
    auto* const snapshot = new (std::nothrow) ustore_snapshot_handle_t {};
    if (!snapshot) return call.refuse(ustore_status_out_of_memory_k, "Failed to allocate the snapshot");
    snapshot->database = &database;
    {
        std::unique_lock lock(database.mutex);
        snapshot->stamp = database.head;
        database.pins.insert(snapshot->stamp);
        ++database.snapshots_open;
    }
    database.holds.fetch_add(1, std::memory_order_acq_rel);
    *arguments->snapshot = snapshot;
    if (arguments->read_stamp) *arguments->read_stamp = snapshot->stamp;
}

void ustore_snapshot_save(ustore_snapshot_save_t* arguments) {
    call_t call(arguments->outcome);
    ustore_snapshot_handle_t* const snapshot = arguments->snapshot;
    if (!snapshot) return call.refuse(ustore_status_uninitialized_k, "The snapshot is not open");
    call.name(&snapshot->named_calls);
    if (refuse_path(call, arguments->path)) return;
    if (!call.hold_executor(arguments->executor)) return;
    std::string const directory(text_of(arguments->path));
    if (directory.empty() || !directory_exists(directory))
        return call.refuse(ustore_status_not_found_k, "The path is no directory");
    try {
        std::string image;
        std::vector<std::size_t> sizes;
        {
            std::shared_lock lock(snapshot->database->mutex);
            build_image(*snapshot->database, snapshot->stamp, image, sizes);
        }
        call.failed_with(store_image(directory, image, sizes, arguments->progress, arguments->progress_state));
    }
    catch (std::bad_alloc const&) {
        call.refuse(ustore_status_out_of_memory_k, "Out of memory while saving");
    }
}

ustore_view_t ustore_snapshot_view(ustore_snapshot_t snapshot) { return tagged_view(snapshot, view_tag_t::snapshot_k); }

void ustore_snapshot_free(ustore_snapshot_t snapshot) {
    if (!snapshot) return;
    wait_named(snapshot->named_calls);
    release_snapshot(snapshot);
}

void ustore_collection_list(ustore_collection_list_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->collections) *arguments->collections = nullptr;
    if (arguments->modalities) *arguments->modalities = nullptr;
    if (arguments->names) *arguments->names = {};
    if (arguments->written) *arguments->written = 0;
    if (refuse_database(call, arguments->database)) return;
    if (!arguments->written)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the count");
    if (!arguments->arena) return call.refuse(ustore_status_invalid_argument_k, "The call needs an arena");
    if (arguments->snapshot &&
        (arguments->snapshot->magic != magic_t::snapshot_k || arguments->snapshot->database != arguments->database))
        return call.refuse(ustore_status_invalid_argument_k, "The snapshot belongs to another database");
    if (arguments->snapshot) call.name(&arguments->snapshot->named_calls);
    if (!call.hold_executor(arguments->executor)) return;
    ustore_arena_handle_t* const arena = claim_arena(call, arguments->arena);
    if (!arena) return;
    ustore_database_handle_t& database = *arguments->database;
    std::vector<ustore_collection_id_t> ids;
    std::vector<ustore_modality_t> modalities;
    std::vector<std::string> names;
    {
        std::shared_lock lock(database.mutex);
        stamp_t const stamp = arguments->snapshot ? arguments->snapshot->stamp : database.head;
        for (auto const& [id, collection] : database.collections) {
            if (collection.created > stamp) continue;
            ids.push_back(collection.id), modalities.push_back(collection.modality), names.push_back(collection.name);
        }
    }
    std::size_t const count = ids.size();
    auto* const ids_out = arguments->collections ? carve<ustore_collection_id_t>(call, *arena, count) : nullptr;
    auto* const modalities_out = arguments->modalities ? carve<ustore_modality_t>(call, *arena, count) : nullptr;
    ustore_strings_view_t names_out {};
    if (arguments->names && count && !write_tape(call, *arena, names, names_out)) return;
    if (call.failed()) return;
    for (std::size_t index = 0; index != count; ++index) {
        if (ids_out) ids_out[index] = ids[index];
        if (modalities_out) modalities_out[index] = modalities[index];
    }
    if (arguments->collections) *arguments->collections = ids_out;
    if (arguments->modalities) *arguments->modalities = modalities_out;
    if (arguments->names) *arguments->names = names_out;
    *arguments->written = count;
}

void ustore_collection_create(ustore_collection_create_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->collection) *arguments->collection = {};
    if (refuse_database(call, arguments->database) || refuse_unbacked(call, arguments->name)) return;
    std::string_view const name = text_of(arguments->name);
    if (std::string_view(ustore_modality_name(arguments->modality)) == "unrecognized")
        return call.refuse(ustore_status_invalid_argument_k, "Unknown modality");
    if (arguments->name.data && name.empty())
        return call.refuse(ustore_status_invalid_argument_k, "A name is never empty");
    if (name.starts_with('_'))
        return call.refuse(ustore_status_invalid_argument_k, "Names starting with _ belong to the system");
    if (!arguments->collection)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the collection");
    if (refuse_read_only(call, arguments->database)) return;
    if (!call.hold_executor(arguments->executor)) return;
    ustore_database_handle_t& database = *arguments->database;
    try {
        std::unique_lock lock(database.mutex);
        if (name_taken(database, name))
            return call.refuse(ustore_status_already_exists_k, "Another collection carries the name");
        collection_t collection;
        std::array<std::uint8_t, 16> const uuid = mint_uuid_v7();
        std::memcpy(collection.id.uuid.bytes, uuid.data(), uuid.size());
        collection.name.assign(name);
        collection.modality = arguments->modality;
        collection.created = ++database.head;
        ustore_collection_id_t const id = collection.id;
        database.collections.emplace(bytes_of(id), std::move(collection));
        // A name lands with its collection in one step that survives a crash, as the
        // image holds both
        if (!database.directory.empty() && call.failed_with(write_image(database, database.head, database.directory)))
            return;
        *arguments->collection = id;
    }
    catch (std::bad_alloc const&) {
        call.refuse(ustore_status_out_of_memory_k, "Out of memory while creating the collection");
    }
}

void ustore_collection_find(ustore_collection_find_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->presences) *arguments->presences = nullptr;
    if (arguments->collections) *arguments->collections = nullptr;
    if (arguments->modalities) *arguments->modalities = nullptr;
    if (refuse_database(call, arguments->database)) return;
    if (!arguments->arena) return call.refuse(ustore_status_invalid_argument_k, "The call needs an arena");
    if (!arguments->collections)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the identifiers");
    strings_in_t names;
    if (!read_strings(call, arguments->names, arguments->count, names)) return;
    if (arguments->count && !names.given)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs the names");
    if (arguments->snapshot &&
        (arguments->snapshot->magic != magic_t::snapshot_k || arguments->snapshot->database != arguments->database))
        return call.refuse(ustore_status_invalid_argument_k, "The snapshot belongs to another database");
    if (arguments->snapshot) call.name(&arguments->snapshot->named_calls);
    if (!call.hold_executor(arguments->executor)) return;
    ustore_arena_handle_t* const arena = claim_arena(call, arguments->arena,
                                                     {first_input(arguments->names), data_input(arguments->names)});
    if (!arena) return;
    std::size_t const count = arguments->count;
    std::vector<ustore_collection_id_t> ids(count);
    std::vector<ustore_modality_t> modalities(count, static_cast<ustore_modality_t>(0));
    std::vector<bool> found(count, false);
    {
        ustore_database_handle_t& database = *arguments->database;
        std::shared_lock lock(database.mutex);
        stamp_t const stamp = arguments->snapshot ? arguments->snapshot->stamp : database.head;
        for (std::size_t index = 0; index != count; ++index) {
            if (names[index].empty()) continue;
            for (auto const& [id, collection] : database.collections)
                if (collection.created <= stamp && collection.name == names[index]) {
                    ids[index] = collection.id, modalities[index] = collection.modality, found[index] = true;
                    break;
                }
        }
    }
    auto* const presences = arguments->presences ? carve<ustore_octet_t>(call, *arena, (count + 7) / 8) : nullptr;
    auto* const ids_out = carve<ustore_collection_id_t>(call, *arena, count);
    auto* const modalities_out = arguments->modalities ? carve<ustore_modality_t>(call, *arena, count) : nullptr;
    if (call.failed()) return;
    for (std::size_t index = 0; index != count; ++index) {
        ids_out[index] = ids[index];
        if (modalities_out) modalities_out[index] = modalities[index];
        if (presences && found[index]) set_bit(presences, index);
    }
    if (arguments->presences) *arguments->presences = presences;
    *arguments->collections = ids_out;
    if (arguments->modalities) *arguments->modalities = modalities_out;
}

void ustore_collection_drop(ustore_collection_drop_t* arguments) {
    call_t call(arguments->outcome);
    if (refuse_database(call, arguments->database)) return;
    if (refuse_drop_mode(call, arguments->mode) || refuse_read_only(call, arguments->database)) return;
    if (!call.hold_executor(arguments->executor)) return;
    ustore_database_handle_t& database = *arguments->database;
    try {
        std::unique_lock lock(database.mutex);
        auto const found = database.collections.find(bytes_of(arguments->collection));
        if (found == database.collections.end())
            return call.refuse(ustore_status_not_found_k, "No collection carries the identifier");
        if (arguments->mode == ustore_drop_values_k && found->second.modality == ustore_modality_strings_k)
            return call.refuse(ustore_status_invalid_argument_k, "A strings collection keeps no other values");
        if (arguments->mode == ustore_drop_collection_k) {
            if (database.snapshots_open)
                return call.refuse(ustore_status_invalid_argument_k, "A snapshot is open while the drop runs");
            database.collections.erase(found);
            ++database.head;
            if (!database.directory.empty()) call.failed_with(write_image(database, database.head, database.directory));
            return;
        }
        batch_t batch;
        stage_emptying(found->second, arguments->mode, batch);
        stamp_t stamp = 0;
        call.failed_with(apply_batch(database, batch, stamp));
    }
    catch (std::bad_alloc const&) {
        call.refuse(ustore_status_out_of_memory_k, "Out of memory while dropping");
    }
}

void ustore_transaction_init(ustore_transaction_init_t* arguments) {
    call_t call(arguments->outcome);
    ustore_transaction_handle_t* const reused = arguments->transaction ? *arguments->transaction : nullptr;
    if (refuse_database(call, arguments->database)) return;
    if (!arguments->transaction)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the transaction");
    if (reused && (reused->magic != magic_t::transaction_k || reused->database != arguments->database))
        return call.refuse(ustore_status_invalid_argument_k, "The transaction belongs to another database");
    if (refuse_read_only(call, arguments->database)) return;
    if (!call.hold_executor(arguments->executor)) return;
    ustore_database_handle_t& database = *arguments->database;
    if (reused) {
        call.name(&reused->named_calls);
        if (!call.claim(&reused->claim, "Another call holds the transaction")) return;
        std::unique_lock lock(database.mutex);
        unpin(database.pins, reused->read_stamp);
        reused->read_stamp = database.head;
        database.pins.insert(reused->read_stamp);
        reused->writes.clear(), reused->reads.clear(), reused->windows.clear();
        reused->staged = false;
        return;
    }
    auto* const transaction = new (std::nothrow) ustore_transaction_handle_t {};
    if (!transaction) return call.refuse(ustore_status_out_of_memory_k, "Failed to allocate the transaction");
    transaction->database = &database;
    {
        std::unique_lock lock(database.mutex);
        transaction->read_stamp = database.head;
        database.pins.insert(transaction->read_stamp);
    }
    database.holds.fetch_add(1, std::memory_order_acq_rel);
    *arguments->transaction = transaction;
}

ustore_view_t ustore_transaction_view(ustore_transaction_t transaction, ustore_watch_t watch) {
    return tagged_view(transaction, watch == ustore_watch_none_k ? view_tag_t::unwatched_k : view_tag_t::watching_k);
}

void ustore_transaction_stage(ustore_transaction_stage_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->read_stamp) *arguments->read_stamp = 0;
    if (!open_transaction_call(call, *arguments)) return;
    ustore_transaction_handle_t& transaction = *arguments->transaction;
    std::shared_lock lock(arguments->database->mutex);
    if (call.failed_with(validate_transaction(*arguments->database, transaction))) return;
    transaction.staged = true;
    if (arguments->read_stamp) *arguments->read_stamp = transaction.read_stamp;
}

void ustore_transaction_commit(ustore_transaction_commit_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->commit_stamp) *arguments->commit_stamp = 0;
    if (!open_transaction_call(call, *arguments)) return;
    ustore_transaction_handle_t& transaction = *arguments->transaction;
    ustore_database_handle_t& database = *arguments->database;
    try {
        std::unique_lock lock(database.mutex);
        transaction.staged = false;
        if (call.failed_with(validate_transaction(database, transaction))) return;
        stamp_t stamp = 0;
        if (call.failed_with(apply_batch(database, transaction.writes, stamp))) return;
        if (arguments->commit_stamp) *arguments->commit_stamp = stamp;
        // The transaction's next generation reads the state its commit published
        unpin(database.pins, transaction.read_stamp);
        transaction.read_stamp = stamp;
        database.pins.insert(stamp);
        transaction.writes.clear(), transaction.reads.clear(), transaction.windows.clear();
        if (arguments->durability == ustore_durability_flushed_k && !database.directory.empty())
            call.failed_with(write_image(database, database.head, database.directory));
    }
    catch (std::bad_alloc const&) {
        call.refuse(ustore_status_out_of_memory_k, "Out of memory while committing");
    }
}

void ustore_transaction_rollback(ustore_transaction_rollback_t* arguments) {
    call_t call(arguments->outcome);
    if (!open_transaction_call(call, *arguments)) return;
    arguments->transaction->staged = false;
}

void ustore_transaction_free(ustore_transaction_t transaction) {
    if (!transaction) return;
    wait_named(transaction->named_calls);
    while (transaction->claim.held()) std::this_thread::yield();
    transaction->writes.clear(), transaction->reads.clear(), transaction->windows.clear();
    release_transaction(transaction);
}

/** What @c ustore_changes_cursor_t names: a feed of one collection's changes and the last one
 *  it handed out. */
struct ustore_changes_cursor_handle_t {
    ustore_database_handle_t* database = nullptr;
    std::string collection;
    stamp_t pinned = 0;
    stamp_t last_stamp = 0;
    std::string last_key;
    bool handed = false;
    claim_t claim;
    named_calls_t named_calls {0};
};

void ustore_changes_cursor_init(ustore_changes_cursor_init_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->cursor) *arguments->cursor = nullptr;
    if (refuse_database(call, arguments->database)) return;
    if (!arguments->cursor)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the cursor");
    if (!call.hold_executor(arguments->executor)) return;
    ustore_database_handle_t& database = *arguments->database;
    auto* const cursor = new (std::nothrow) ustore_changes_cursor_handle_t {};
    if (!cursor) return call.refuse(ustore_status_out_of_memory_k, "Failed to allocate the cursor");
    {
        std::unique_lock lock(database.mutex);
        auto const found = database.collections.find(bytes_of(arguments->collection));
        if (found == database.collections.end()) {
            delete cursor;
            return call.refuse(ustore_status_not_found_k, "No such collection");
        }
        bool const covered = !database.feed_pins.empty() && *database.feed_pins.begin() <= arguments->since_stamp;
        if (arguments->since_stamp < database.head && !covered) {
            delete cursor;
            return call.refuse(ustore_status_write_conflict_k, "The versions after the stamp are already gone");
        }
        cursor->database = &database;
        cursor->collection = found->first;
        cursor->pinned = arguments->since_stamp;
        cursor->last_stamp = arguments->since_stamp;
        database.feed_pins.insert(cursor->pinned);
        database.pins.insert(cursor->pinned);
    }
    database.holds.fetch_add(1, std::memory_order_acq_rel);
    *arguments->cursor = cursor;
}

void ustore_changes_cursor_next(ustore_changes_cursor_next_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->count) *arguments->count = 0;
    if (arguments->keys) *arguments->keys = nullptr;
    if (arguments->stamps) *arguments->stamps = nullptr;
    if (arguments->kinds) *arguments->kinds = nullptr;
    if (arguments->values) *arguments->values = {};
    ustore_changes_cursor_handle_t* const cursor = arguments->cursor;
    if (!cursor || !arguments->arena || !arguments->count || !arguments->keys || !arguments->stamps ||
        !arguments->kinds)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs a cursor, an arena and outputs");
    call.name(&cursor->named_calls);
    if (!call.hold_executor(arguments->executor)) return;
    ustore_arena_handle_t* const arena = claim_arena(call, arguments->arena);
    if (!arena || !call.claim(&cursor->claim, "Another call pulls the cursor")) return;
    ustore_database_handle_t& database = *cursor->database;
    std::vector<change_t> pulled;
    ustore_modality_t modality = ustore_modality_blobs_k;
    try {
        std::unique_lock lock(database.mutex);
        auto const found = database.collections.find(cursor->collection);
        if (found == database.collections.end())
            return call.refuse(ustore_status_not_found_k, "The collection was dropped meanwhile");
        modality = found->second.modality;
        for (change_t const& change : found->second.changes) {
            if (pulled.size() == arguments->wanted) break;
            bool const after = change.stamp > cursor->last_stamp ||
                               (cursor->handed && change.stamp == cursor->last_stamp && change.key > cursor->last_key);
            if (after) pulled.push_back(change);
        }
        if (!pulled.empty()) {
            cursor->last_stamp = pulled.back().stamp, cursor->last_key = pulled.back().key, cursor->handed = true;
            // The commit of the last change may hold more, so the pin stays just below it
            unpin(database.feed_pins, cursor->pinned), unpin(database.pins, cursor->pinned);
            cursor->pinned = cursor->last_stamp - 1;
            database.feed_pins.insert(cursor->pinned), database.pins.insert(cursor->pinned);
        }
    }
    catch (std::bad_alloc const&) {
        return call.refuse(ustore_status_out_of_memory_k, "Out of memory while pulling");
    }
    std::size_t const count = pulled.size();
    auto* const keys = carve<ustore_key_t>(call, *arena, count);
    auto* const stamps = carve<ustore_stamp_t>(call, *arena, count);
    auto* const kinds = carve<ustore_change_kind_t>(call, *arena, count);
    std::vector<std::string_view> texts;
    for (std::size_t index = 0; index != count; ++index) {
        change_t const& change = pulled[index];
        // Strings name their changes by the key they map onto, and graphs by the vertex the
        // half-edge files under
        keys[index] = modality == ustore_modality_strings_k ? key_of(change.value) : key_of(change.key);
        stamps[index] = change.stamp, kinds[index] = change.kind;
        texts.push_back(modality == ustore_modality_strings_k || modality == ustore_modality_graph_k
                            ? std::string_view(change.key)
                            : std::string_view(change.value));
    }
    ustore_strings_view_t values {};
    if (arguments->values && count && !write_tape(call, *arena, texts, values)) return;
    if (call.failed()) return;
    *arguments->count = count;
    *arguments->keys = keys, *arguments->stamps = stamps, *arguments->kinds = kinds;
    if (arguments->values) *arguments->values = values;
}

void ustore_changes_cursor_free(ustore_changes_cursor_t cursor) {
    if (!cursor) return;
    wait_named(cursor->named_calls);
    while (cursor->claim.held()) std::this_thread::yield();
    ustore_database_handle_t* const database = cursor->database;
    {
        std::unique_lock lock(database->mutex);
        unpin(database->feed_pins, cursor->pinned), unpin(database->pins, cursor->pinned);
    }
    delete cursor;
    release_database(database);
}

/** What @c ustore_ingest_t names: the rows appended so far, keyed as the collection keys them. */
struct ustore_ingest_handle_t {
    ustore_database_handle_t* database = nullptr;
    std::string collection;
    ustore_modality_t modality = ustore_modality_blobs_k;
    ustore_ingest_order_t order = ustore_ingest_sorted_k;
    bool finished = false;
    overlay_t rows;
    std::string last_key;
    bool appended = false;
    claim_t claim;
};

void ustore_ingest_init(ustore_ingest_init_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->ingest) *arguments->ingest = nullptr;
    if (refuse_database(call, arguments->database)) return;
    if (!arguments->ingest)
        return call.refuse(ustore_status_invalid_argument_k, "The call needs an output for the ingest");
    auto const order = std::bit_cast<std::underlying_type_t<ustore_ingest_order_t>>(arguments->order);
    if (order != ustore_ingest_sorted_k && order != ustore_ingest_unsorted_k)
        return call.refuse(ustore_status_invalid_argument_k, "Unknown ingest order");
    if (refuse_read_only(call, arguments->database)) return;
    ustore_database_handle_t& database = *arguments->database;
    auto* const ingest = new (std::nothrow) ustore_ingest_handle_t {};
    if (!ingest) return call.refuse(ustore_status_out_of_memory_k, "Failed to allocate the ingest");
    {
        std::shared_lock lock(database.mutex);
        auto const found = database.collections.find(bytes_of(arguments->collection));
        if (found == database.collections.end()) {
            delete ingest;
            return call.refuse(ustore_status_not_found_k, "No such collection");
        }
        ingest->collection = found->first;
        ingest->modality = found->second.modality;
    }
    ingest->database = &database;
    ingest->order = arguments->order;
    database.holds.fetch_add(1, std::memory_order_acq_rel);
    *arguments->ingest = ingest;
}

/** The rows an ingest appends for edge @p index of @p arguments: both vertices, then both
 *  half-edges. */
void key_edge_rows(ustore_ingest_append_t const& arguments, std::size_t index,
                   std::vector<std::pair<std::string, std::string>>& rows) {
    ustore_key_t const& source = strided_element(arguments.sources.data, arguments.sources.stride, index);
    ustore_key_t const& target = strided_element(arguments.targets.data, arguments.targets.stride, index);
    ustore_key_t const edge = arguments.edges.data
                                  ? strided_element(arguments.edges.data, arguments.edges.stride, index)
                                  : ustore_edge_id_default_k;
    rows.emplace_back(vertex_entry(source), std::string {});
    rows.emplace_back(vertex_entry(target), std::string {});
    rows.emplace_back(half_edge(source, 1, target, edge), std::string {});
    rows.emplace_back(half_edge(target, 2, source, edge), std::string {});
}

/** Keys the row of @p value an ingest into a collection of @p modality appends under @p key,
 *  checking a document parses and importing a page. */
status_t key_row(ustore_modality_t modality, ustore_key_t const& key, std::string_view value,
                 std::vector<std::pair<std::string, std::string>>& rows) {
    std::string stored;
    switch (modality) {
    case ustore_modality_strings_k: rows.emplace_back(std::string(value), bytes_of(key)); return {};
    case ustore_modality_docs_k:
        if (status_t const status = check_document(value); status.failed()) return status;
        rows.emplace_back(bytes_of(key), std::string(value));
        return {};
    case ustore_modality_pages_k:
        if (status_t const status = import_page(value, key, stored); status.failed()) return status;
        rows.emplace_back(bytes_of(key), std::move(stored));
        return {};
    default: rows.emplace_back(bytes_of(key), std::string(value)); return {};
    }
}

/** Whether @p rows ascend strictly past every key @p ingest appended before them. */
bool rows_ascend(ustore_ingest_handle_t const& ingest, std::vector<std::pair<std::string, std::string>> const& rows) {
    std::string const* last = ingest.appended ? &ingest.last_key : nullptr;
    for (auto const& row : rows) {
        if (last && !(*last < row.first)) return false;
        last = &row.first;
    }
    return true;
}

void ustore_ingest_append(ustore_ingest_append_t* arguments) {
    call_t call(arguments->outcome);
    ustore_ingest_handle_t* const ingest = arguments->ingest;
    if (!ingest || ingest->finished)
        return call.refuse(ustore_status_invalid_argument_k, "The ingest is null or finished");
    std::size_t const count = arguments->count;
    bool const edges = ingest->modality == ustore_modality_graph_k;
    strings_in_t values;
    if (edges && count && (!arguments->sources.data || !arguments->targets.data))
        return call.refuse(ustore_status_invalid_argument_k, "Edges need their sources and targets");
    // The accessor of the values runs inside the append, which holds the ingest meanwhile
    if (!call.claim(&ingest->claim, "Another append or the finish runs on the ingest")) return;
    if (!edges) {
        if (!read_strings(call, arguments->values, count, values)) return;
        if (count && (!arguments->keys.data || !values.given))
            return call.refuse(ustore_status_invalid_argument_k, "Rows need their keys and values");
    }
    try {
        // Every row is checked and keyed before any lands, so a failed append leaves the ingest
        // as it was
        std::vector<std::pair<std::string, std::string>> rows;
        rows.reserve(edges ? 4 * count : count);
        for (std::size_t index = 0; index != count && edges; ++index) key_edge_rows(*arguments, index, rows);
        for (std::size_t index = 0; index != count && !edges; ++index) {
            ustore_key_t const& key = strided_element(arguments->keys.data, arguments->keys.stride, index);
            if (call.failed_with(key_row(ingest->modality, key, values[index], rows))) return;
        }
        if (ingest->order == ustore_ingest_sorted_k && !edges && !rows_ascend(*ingest, rows))
            return call.refuse(ustore_status_invalid_argument_k, "A key does not ascend past every key before it");
        for (auto& [key, value] : rows) ingest->rows[key] = {true, std::move(value), ustore_change_write_k, true};
        if (!rows.empty() && !edges) ingest->last_key = rows.back().first, ingest->appended = true;
    }
    catch (std::bad_alloc const&) {
        call.refuse(ustore_status_out_of_memory_k, "Out of memory while appending");
    }
}

void ustore_ingest_finish(ustore_ingest_finish_t* arguments) {
    call_t call(arguments->outcome);
    if (arguments->commit_stamp) *arguments->commit_stamp = 0;
    ustore_ingest_handle_t* const ingest = arguments->ingest;
    if (!ingest || ingest->finished)
        return call.refuse(ustore_status_invalid_argument_k, "The ingest is null or finished");
    auto const durability = std::bit_cast<std::underlying_type_t<ustore_durability_t>>(arguments->durability);
    if (durability != ustore_durability_buffered_k && durability != ustore_durability_flushed_k)
        return call.refuse(ustore_status_invalid_argument_k, "Unknown durability");
    if (!call.claim(&ingest->claim, "An append runs on the ingest")) return;
    ingest->finished = true;
    ustore_database_handle_t& database = *ingest->database;
    try {
        std::unique_lock lock(database.mutex);
        if (!database.collections.count(ingest->collection))
            return call.refuse(ustore_status_not_found_k, "The collection was dropped meanwhile");
        batch_t batch;
        batch[ingest->collection] = std::move(ingest->rows);
        stamp_t stamp = 0;
        if (call.failed_with(apply_batch(database, batch, stamp))) return;
        if (arguments->commit_stamp) *arguments->commit_stamp = stamp;
        if (arguments->durability == ustore_durability_flushed_k && !database.directory.empty())
            call.failed_with(write_image(database, database.head, database.directory));
    }
    catch (std::bad_alloc const&) {
        call.refuse(ustore_status_out_of_memory_k, "Out of memory while landing the ingest");
    }
}

void ustore_ingest_free(ustore_ingest_t ingest) {
    if (!ingest) return;
    while (ingest->claim.held()) std::this_thread::yield();
    ustore_database_handle_t* const database = ingest->database;
    delete ingest;
    release_database(database);
}
