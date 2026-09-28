/**
 *  @file src/nvme/engine.cpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Embedded on-disk engine over UStoreNVMe, one LSM collection per collection of the C ABI.
 *
 *  One mount opens the volumes, one database names the collections that live on them, and every
 *  exported call resolves the collection it addresses and runs against the store beneath it. The
 *  transport is whichever file transport the probe settles on, because the database this ABI names
 *  materializes over volumes kept in files.
 *
 *  Two collections are reserved and created on the first mount: the one @c ustore_collection_main_k
 *  addresses and the one the strings modality files its names under. Every other collection is
 *  addressed by the number the catalog gave it.
 *
 *  Ordered access is a merge over the runs of one held level set, since a store of sorted runs offers
 *  no cursor of its own. A snapshot is taken, walked and dropped inside the call that needs it, so no
 *  publication ever waits on a hold this engine still owns.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm>   // `std::max`, `std::min`
#include <chrono>      // `std::chrono::steady_clock`
#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::remove_reference_t`
#include <utility>     // `std::in_range`, `std::move`
#include <variant>     // `std::visit`

#include <fmt/format.h> // `fmt::format_to_n`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/blobs.h>
#include <ustore/db.h>
#include <ustore/ranges_args.hpp> // `places_arg_t`, `contents_arg_t`, `scans_arg_t`, `validate_read`

#include "engine.hpp"                // `engine_t`, `handle_t`, `handle_cast`
#include "helpers/linked_array.hpp"  // `uninitialized_array`
#include "helpers/linked_memory.hpp" // `linked_memory`, `clear_linked_memory`

#include "nvme/collection_snapshot.hpp" // `select_result_t`
#include "nvme/config.hpp"              // `resources_config_t`, `user_collection_config_t`, `validate`
#include "nvme/database.hpp"            // `database`
#include "nvme/filesystem.hpp"          // `basic_storage_mount`, `join_path`, `path_max_bytes_k`
#include "nvme/resources.hpp"           // `open_resources`, `resources_shared_ptr_t`

namespace unum::ustore::nvme {

#pragma region Store

/** The key every collection of this engine is addressed by, which is the key of the C ABI. */
using stored_key_t = ustore_key_t;

/** The store every call runs against, whose volumes live in files. */
using store_t = database<stored_key_t>;

/** The mount those volumes are opened over, which the store borrows a router and channels from. */
using storage_mount_t = mounted_storage_t::files_mount_t;

/** One transaction over the collections of the store. */
using store_transaction_t = typename store_t::transaction_t;

/** A collection named by its number, which is how this engine addresses every one of them. */
using any_collection_id_t = typename store_t::any_collection_id_t;

/** One level set per collection, held for the length of one ordered walk. */
using store_snapshot_t = typename store_t::snapshot_t;

/** The name of the collection @c ustore_collection_main_k addresses. */
inline constexpr std::string_view main_collection_name_k = "main";

/** The name of the collection the strings modality files its names under. */
inline constexpr std::string_view names_collection_name_k = "names";

/** The directory a mount opens its only volume in, under the database's own directory. */
inline constexpr char data_directory_name_k[] = "data";

/** Bytes the only volume of a database spans when its configuration names no capacity. */
inline constexpr std::size_t default_volume_bytes_k = 1024ull * 1024ull * 1024ull;

/** Spare channels a mount keeps, of which a compaction holds two and every other call one. */
inline constexpr std::size_t engine_shared_io_channels_k = 4;

/** Bytes the largest value of a collection this engine creates takes. */
inline constexpr stored_value_size_t engine_value_max_size_k = 1024u * 1024u;

/** Bytes one collection's write buffer and one collection's value cache each hold. */
inline constexpr std::size_t engine_collection_buffer_bytes_k = 16ull * 1024ull * 1024ull;

/** Writes one collection's write buffer holds at once. */
inline constexpr std::size_t engine_collection_buffer_elements_k = 64ull * 1024ull;

/** Bytes level zero of a collection admits before a compaction is due. */
inline constexpr std::size_t engine_first_level_bytes_k = 16ull * 1024ull * 1024ull;

/** Bytes one transaction's write buffer holds, which is charged eagerly when the transaction opens. */
inline constexpr std::size_t engine_transaction_bytes_k = 4ull * 1024ull * 1024ull;

/** Writes one transaction's write buffer holds at once. */
inline constexpr std::size_t engine_transaction_elements_k = 16ull * 1024ull;

/** Reports a failed @p status through @p error, answering whether it failed. */
inline bool reported_failure(status_t status, ustore_error_t* error) noexcept {
    if (status.succeeded()) return false;
    *error = status;
    return true;
}

/** The shape every collection this engine creates takes, whose values differ in width. */
[[nodiscard]] inline user_collection_config_t engine_collection_defaults() noexcept {
    user_collection_config_t config;
    config.concurrency_limit = 32;
    config.cache_limit = engine_collection_buffer_bytes_k;
    config.write_buffer_max_elements = engine_collection_buffer_elements_k;
    config.write_buffer_max_bytes = engine_collection_buffer_bytes_k;
    config.first_level_max_bytes = engine_first_level_bytes_k;
    config.level_enlarge_factor = 4;
    config.value_size = 0;
    config.value_max_size = engine_value_max_size_k;
    return config;
}

/** The transaction bounds every transaction of this engine takes, which its buffer is sized by. */
[[nodiscard]] inline database_config_t engine_database_defaults() noexcept {
    database_config_t config;
    config.default_transaction.write_buffer_max_bytes = engine_transaction_bytes_k;
    config.default_transaction.write_buffer_max_elements = engine_transaction_elements_k;
    return config;
}

#pragma endregion Store

#pragma region Handles

/** The calls this engine serves, which stamp every handle it hands out. */
extern engine_t const engine_k;

/** Everything one open database of the C ABI holds. */
struct opened_database_t : handle_t {

    /** The budgets, the executor and the mount the store borrows its router and channels from. */
    resources_shared_ptr_t resources;

    /** The store itself, unmounted before the mount underneath it is torn down. */
    std::unique_ptr<store_t> store;

    /** The shape every collection this database creates takes. */
    user_collection_config_t collection_defaults;

    /** The number the collection @c ustore_collection_main_k addresses answers to. */
    collection_numeric_id_t main = collection_invalid_numeric_id_k;

    /** The number the collection of the strings modality answers to. */
    collection_numeric_id_t names = collection_invalid_numeric_id_k;
};

/** A transaction handle of the C ABI. */
struct opened_transaction_t : handle_t {

    /** The database the transaction belongs to, which outlives it. */
    opened_database_t* database = nullptr;

    /** The transaction itself, retired when it dies. */
    typename store_t::transaction_unique_ptr_t held;
};

/** The database behind @p handle. */
[[nodiscard]] inline opened_database_t& database_of(ustore_database_t handle) noexcept {
    return *handle_cast<opened_database_t>(handle);
}

/** The transaction behind @p handle, or null when the call names none. */
[[nodiscard]] inline opened_transaction_t* transaction_of(ustore_transaction_t handle) noexcept {
    return handle_cast<opened_transaction_t>(handle);
}

#pragma region Strings and Names

/** The collection @c nvme/strings.cpp keeps the names of collections in, above every identifier handed out. */
extern ustore_collection_t const names_collection_k;

// Laid over NVMe's keyed calls in `nvme/strings.cpp`, until NVMe keys strings and names itself
void strings_write(ustore_strings_write_t& arguments) noexcept;
void strings_read(ustore_strings_read_t& arguments) noexcept;
void strings_scan(ustore_strings_scan_t& arguments) noexcept;
void collection_find(ustore_collection_find_t& arguments) noexcept;
void with_names(ustore_collection_create_t& arguments, void (*create)(ustore_collection_create_t&) noexcept) noexcept;
void with_names(ustore_collection_drop_t& arguments, void (*drop)(ustore_collection_drop_t&) noexcept) noexcept;
void with_names(ustore_collection_list_t& arguments, void (*list)(ustore_collection_list_t&) noexcept) noexcept;

/** The table entry serving @p native_, NVMe's own call that knows no names, with the names of collections. */
template <typename arguments_type_, void (*native_)(arguments_type_&) noexcept>
void with_names(arguments_type_& arguments) noexcept {
    with_names(arguments, native_);
}

#pragma endregion Strings and Names

/** The number @p collection resolves to in @p database, reserved identifiers included. */
[[nodiscard]] inline collection_numeric_id_t numbered(opened_database_t const& database,
                                                      ustore_collection_t collection) noexcept {
    if (collection == ustore_collection_main_k) return database.main;
    if (collection == names_collection_k) return database.names;
    return static_cast<collection_numeric_id_t>(collection);
}

/** Whether @p numeric_id names one of the two collections the engine reserves for itself. */
[[nodiscard]] inline bool reserved(opened_database_t const& database, collection_numeric_id_t numeric_id) noexcept {
    return numeric_id == database.main || numeric_id == database.names;
}

/** The watches a call of @p options takes, which a caller turns off per call. */
[[nodiscard]] inline watch_schedule_t schedule_of(ustore_options_t options) noexcept {
    return options & ustore_option_transaction_dont_watch_k ? watch_schedule_t::never_k : watch_schedule_t::on_commit_k;
}

#pragma endregion Handles

#pragma region Transaction Dispatch

/**
 *  @brief Reads @p stored_key of @p numeric_id through @p handler, watching it as @p schedule asks.
 *
 *  The transaction's own buffer is asked first and only falls through when it carries no value, since
 *  the store's own fall-through would answer a key the transaction erased with the committed one.
 */
inline status_t read_through(store_transaction_t const& handler, watch_schedule_t schedule,
                             collection_numeric_id_t numeric_id, stored_key_t stored_key, std::span<byte_t> into,
                             stored_value_size_t& size) noexcept {
    any_collection_id_t const id {numeric_id};
    if (handler.contains_uncommitted(id, stored_key)) return handler.select_uncommitted(id, stored_key, into, size);
    if (schedule == watch_schedule_t::never_k)
        return handler.select_committed<watch_schedule_t::never_k>(id, stored_key, into, size);
    return handler.select_committed<watch_schedule_t::on_commit_k>(id, stored_key, into, size);
}

/** Stages @p stored_value under @p stored_key of @p numeric_id, watching it as @p schedule asks. */
inline status_t write_through(store_transaction_t& handler, watch_schedule_t schedule,
                              collection_numeric_id_t numeric_id, stored_key_t stored_key,
                              stored_value_view_t stored_value) noexcept {
    any_collection_id_t const id {numeric_id};
    if (schedule == watch_schedule_t::never_k)
        return handler.insert<watch_schedule_t::never_k>(id, stored_key, stored_value);
    return handler.insert<watch_schedule_t::on_commit_k>(id, stored_key, stored_value);
}

/** Stages a removal of @p stored_key of @p numeric_id, watching it as @p schedule asks. */
inline status_t erase_through(store_transaction_t& handler, watch_schedule_t schedule,
                              collection_numeric_id_t numeric_id, stored_key_t stored_key) noexcept {
    any_collection_id_t const id {numeric_id};
    if (schedule == watch_schedule_t::never_k) return handler.remove<watch_schedule_t::never_k>(id, stored_key);
    return handler.remove<watch_schedule_t::on_commit_k>(id, stored_key);
}

#pragma endregion Transaction Dispatch

#pragma region Ordered Walk

/** Whether a walk carries on past the key it was handed. */
enum class walk_control_t : bool { halt_k, resume_k };

/** Where one run of one held level set has got to. */
struct run_cursor_t {

    /** The level the run belongs to, zero being the one flushes land on. */
    std::uint32_t level_number = 0;

    /** The run inside that level, older first where the runs overlap. */
    std::uint32_t run_index = 0;

    /** The next sorted position inside that run. */
    std::uint64_t position = 0;

    /** Key slots the run holds, past which the cursor is spent. */
    std::uint64_t population = 0;
};

/** Whether @p first carries a newer version of a key than @p second does. */
[[nodiscard]] inline bool outranks(run_cursor_t const& first, run_cursor_t const& second) noexcept {
    if (first.level_number != second.level_number) return first.level_number < second.level_number;
    return first.run_index > second.run_index;
}

/** The first position of @p run whose key is not below @p from. */
template <typename run_type_>
[[nodiscard]] std::uint64_t lower_bound_position(run_type_ const& run, stored_key_t from) noexcept {
    std::uint64_t low = 0;
    std::uint64_t high = run.population_count();
    while (low != high) {
        std::uint64_t const middle = low + (high - low) / 2;
        if (run.key_slot(middle).stored_key < from) low = middle + 1;
        else high = middle;
    }
    return low;
}

/**
 *  @brief Calls @p visitor with every key of @p held from @p from up, in ascending order.
 *  @param[in] held The level set of one collection, which a flush already published into.
 *  @param[in] from The smallest key the walk reports, which need not be in the collection.
 *  @param[in] visitor Called as @c visitor(key, state, size) and answering whether the walk goes on.
 *
 *  @note Only the newest version of a key is reported, so an erasure hides every run behind it.
 */
template <typename collection_snapshot_type_, typename visitor_type_>
status_t walk_from(collection_snapshot_type_ const& held, stored_key_t from, visitor_type_&& visitor) noexcept {
    auto const& levels = held.levels();
    std::size_t runs = 0;
    for (std::size_t number = 0; number != levels.size(); ++number) runs += levels.level(number).size();

    st::basic_vector<run_cursor_t> cursors;
    if (cursors.reserve(runs) != st::status_t::success_k)
        return {status_code_t::out_of_memory_k, "A walk could not hold a cursor per run"};
    for (std::size_t number = 0; number != levels.size(); ++number) {
        auto const& level = levels.level(number);
        for (std::size_t index = 0; index != level.size(); ++index) {
            run_cursor_t cursor;
            cursor.level_number = static_cast<std::uint32_t>(number);
            cursor.run_index = static_cast<std::uint32_t>(index);
            cursor.population = level.run(index).population_count();
            cursor.position = lower_bound_position(level.run(index), from);
            [[maybe_unused]] st::status_t const placed = cursors.push_back(st::assume_reserved, std::move(cursor));
        }
    }

    auto const key_slot = [&](run_cursor_t const& cursor) noexcept -> auto const& {
        return levels.level(cursor.level_number).run(cursor.run_index).key_slot(cursor.position);
    };

    for (;;) {
        stored_key_t smallest = limit<stored_key_t>::max();
        bool any = false;
        for (std::size_t index = 0; index != cursors.size(); ++index) {
            if (cursors[index].position == cursors[index].population) continue;
            stored_key_t const head = key_slot(cursors[index]).stored_key;
            if (any && !(head < smallest)) continue;
            smallest = head;
            any = true;
        }
        if (!any) return {};

        std::size_t chosen = cursors.size();
        for (std::size_t index = 0; index != cursors.size(); ++index) {
            if (cursors[index].position == cursors[index].population) continue;
            if (!(key_slot(cursors[index]).stored_key == smallest)) continue;
            if (chosen != cursors.size() && !outranks(cursors[index], cursors[chosen])) continue;
            chosen = index;
        }
        auto const& slot = key_slot(cursors[chosen]);
        select_result_t const state = slot.is_tombstone() ? select_result_t::erased_k : select_result_t::success_k;
        stored_value_size_t const size = slot.is_tombstone() ? 0 : slot.size;

        for (std::size_t index = 0; index != cursors.size(); ++index)
            if (cursors[index].position != cursors[index].population && key_slot(cursors[index]).stored_key == smallest)
                ++cursors[index].position;

        if (visitor(smallest, state, size) == walk_control_t::halt_k) return {};
    }
}

/** Walks the collection @p numeric_id names inside @p taken, which walks nothing when it holds none. */
template <typename visitor_type_>
status_t walk_collection(store_snapshot_t const& taken, collection_numeric_id_t numeric_id, stored_key_t from,
                         visitor_type_&& visitor) noexcept {
    for (std::size_t index = 0; index != taken.size(); ++index) {
        if (taken.at(index).numeric_id != numeric_id) continue;
        return std::visit([&](auto const& held) noexcept { return walk_from(held, from, visitor); },
                          taken.at(index).levels);
    }
    return {};
}

/** Appends every key of @p numeric_id from @p from up to @p into, which a mutation then works through. */
inline status_t collect_keys(store_snapshot_t const& taken, collection_numeric_id_t numeric_id, stored_key_t from,
                             st::basic_vector<stored_key_t>& into) noexcept {
    status_t room;
    status_t const walked = walk_collection(
        taken, numeric_id, from,
        [&](stored_key_t key, select_result_t state, stored_value_size_t) noexcept -> walk_control_t {
            if (state != select_result_t::success_k) return walk_control_t::resume_k;
            if (into.push_back(stored_key_t {key}) == st::status_t::success_k) return walk_control_t::resume_k;
            room = {status_code_t::out_of_memory_k, "A walk could not hold the keys it found"};
            return walk_control_t::halt_k;
        });
    return walked.failed() ? walked : room;
}

#pragma endregion Ordered Walk

#pragma region Output Tapes

/** The columns of batched scans and samples: offsets and counts per task over one tape of keys. */
class key_tape_t {
    ustore_error_t* error_;
    range_or_dummy<std::span<ustore_length_t>> offsets_;
    range_or_dummy<std::span<ustore_length_t>> counts_;
    uninitialized_array<ustore_key_t> keys_;
    std::size_t tasks_;

  public:
    key_tape_t(linked_memory_lock_t& arena, std::size_t tasks, ustore_length_t** offsets, ustore_length_t** counts,
               ustore_error_t* error) noexcept
        : error_(error), offsets_(arena.alloc_or_dummy(tasks + 1, error, offsets)),
          counts_(arena.alloc_or_dummy(tasks, error, counts)), keys_(arena), tasks_(tasks) {}

    /** Sizes the tape for @p keys keys, so a walk within it never moves the tape. */
    void reserve(std::size_t keys) noexcept { keys_.reserve(keys, error_); }

    /** How many keys the tape holds. */
    [[nodiscard]] std::size_t size() const noexcept { return keys_.size(); }

    /** The key at @p index. */
    [[nodiscard]] ustore_key_t& operator[](std::size_t index) noexcept { return keys_[index]; }

    /** Appends @p key, halting the walk when the arena refuses. */
    walk_control_t push(ustore_key_t key) noexcept {
        keys_.push_back(key, error_);
        return error_->status ? walk_control_t::halt_k : walk_control_t::resume_k;
    }

    /** Records where task @p task starts and how many keys it holds, from @p first on. */
    void close_task(std::size_t task, std::size_t first) noexcept {
        offsets_[task] = static_cast<ustore_length_t>(first);
        counts_[task] = static_cast<ustore_length_t>(keys_.size() - first);
    }

    /** Closes the offsets and exports the tape of keys. */
    void finish(ustore_key_t** keys) noexcept {
        offsets_[tasks_] = static_cast<ustore_length_t>(keys_.size());
        *keys = keys_.begin();
    }
};

/** A splitmix64 generator, drawing the positions a reservoir keeps. */
struct sample_generator_t {

    /** The state every draw advances. */
    std::uint64_t state;

    std::uint64_t operator()() noexcept {
        std::uint64_t mixed = (state += 0x9E3779B97F4A7C15ull);
        mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
        mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
        return mixed ^ (mixed >> 31);
    }
};

#pragma endregion Output Tapes

#pragma region Argument Views

/** The locations of a read. */
[[nodiscard]] inline places_arg_t places_of(ustore_read_t const& arguments) noexcept {
    return {{arguments.collections, arguments.collections_stride},
            {arguments.keys, arguments.keys_stride},
            {},
            arguments.tasks_count};
}

/** The locations of a write. */
[[nodiscard]] inline places_arg_t places_of(ustore_write_t const& arguments) noexcept {
    return {{arguments.collections, arguments.collections_stride},
            {arguments.keys, arguments.keys_stride},
            {},
            arguments.tasks_count};
}

/** The values of a write. */
[[nodiscard]] inline contents_arg_t contents_of(ustore_write_t const& arguments) noexcept {
    return {bits_view_t {arguments.presences},
            {arguments.offsets, arguments.offsets_stride},
            {arguments.lengths, arguments.lengths_stride},
            {arguments.values, arguments.values_stride},
            arguments.tasks_count};
}

/** The tasks of a scan. */
[[nodiscard]] inline scans_arg_t scans_of(ustore_scan_t const& arguments) noexcept {
    return {{arguments.collections, arguments.collections_stride},
            {arguments.start_keys, arguments.start_keys_stride},
            {arguments.count_limits, arguments.count_limits_stride},
            arguments.tasks_count};
}

/** The tasks of a sample. */
[[nodiscard]] inline sample_args_t samples_of(ustore_sample_t const& arguments) noexcept {
    return {{arguments.collections, arguments.collections_stride},
            {arguments.count_limits, arguments.count_limits_stride},
            arguments.tasks_count};
}

/** The keys @p scans may return together, capped so a huge limit reserves nothing it may never fill. */
[[nodiscard]] inline std::size_t expected_keys(scans_arg_t const& scans) noexcept {
    constexpr std::size_t reservation_cap_k = std::size_t {1} << 16;
    std::size_t expected = 0;
    for (std::size_t task = 0; task != scans.size() && expected < reservation_cap_k; ++task)
        expected += scans[task].limit;
    return std::min(expected, reservation_cap_k);
}

#pragma endregion Argument Views

#pragma region Batches

/** One collection of a batch and the requests of the batch that name it. */
struct batch_group_t {

    /** The number the store addresses the collection by. */
    collection_numeric_id_t numeric_id = collection_invalid_numeric_id_k;

    /** The keys of this collection, in the order the batch names them. */
    st::basic_vector<stored_key_t> keys;

    /** The request of the batch every key came from, one per key. */
    st::basic_vector<std::size_t> requests;
};

/**
 *  @brief Groups @p places by the collection they address, since a batched call takes one at a time.
 *
 *  A read of several collections is several reads of the store, and the store takes its keys
 *  contiguous, so the batch is laid out per collection rather than walked per place.
 */
inline status_t group_places(opened_database_t const& database, places_arg_t const& places,
                             st::basic_vector<batch_group_t>& groups) noexcept {
    for (std::size_t index = 0; index != places.size(); ++index) {
        place_t const place = places[index];
        collection_numeric_id_t const numeric_id = numbered(database, place.collection);
        std::size_t found = groups.size();
        for (std::size_t group = 0; group != groups.size(); ++group)
            if (groups[group].numeric_id == numeric_id) found = group;
        if (found == groups.size()) {
            batch_group_t fresh;
            fresh.numeric_id = numeric_id;
            if (groups.push_back(std::move(fresh)) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A batch could not hold another collection"};
        }
        if (groups[found].keys.push_back(stored_key_t {place.key}) != st::status_t::success_k ||
            groups[found].requests.push_back(std::size_t {index}) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A batch could not hold another key"};
    }
    return {};
}

#pragma endregion Batches

#pragma region Reads

/**
 *  @brief Bytes @p stored_key takes as @p handler sees it, or @c ustore_length_missing_k for a miss.
 *
 *  A key the transaction erased itself sits in its buffer as a tombstone, which the store's own size
 *  call reports as a value of no bytes, so the buffer is asked whether it carries a value first.
 */
[[nodiscard]] inline ustore_length_t measured_in_transaction(store_transaction_t const& handler,
                                                             collection_numeric_id_t numeric_id,
                                                             stored_key_t stored_key) noexcept {
    any_collection_id_t const id {numeric_id};
    stored_value_size_t size = 0;
    if (handler.stored_value_size_uncommitted(id, stored_key, size).succeeded())
        return handler.contains_uncommitted(id, stored_key) ? static_cast<ustore_length_t>(size)
                                                            : ustore_length_missing_k;
    if (handler.stored_value_size_committed(id, stored_key, size).succeeded())
        return static_cast<ustore_length_t>(size);
    return ustore_length_missing_k;
}

/** Bytes the value of @p stored_key takes, or @c ustore_length_missing_k for a miss and an erasure. */
[[nodiscard]] inline ustore_length_t measured_length(opened_database_t const& database,
                                                     opened_transaction_t const* transaction,
                                                     collection_numeric_id_t numeric_id,
                                                     stored_key_t stored_key) noexcept {
    if (transaction) return measured_in_transaction(*transaction->held, numeric_id, stored_key);
    stored_value_size_t size = 0;
    status_t const answered = database.store->stored_value_size(any_collection_id_t {numeric_id}, stored_key, size);
    return answered.succeeded() ? static_cast<ustore_length_t>(size) : ustore_length_missing_k;
}

/** Reads every group of @p groups into @p tape, whose regions @p starts locates per request. */
inline status_t read_batched(opened_database_t& database, st::basic_vector<batch_group_t> const& groups,
                             std::span<ustore_length_t const> starts, std::span<ustore_length_t const> lengths,
                             std::span<byte_t> tape) noexcept {
    for (std::size_t group = 0; group != groups.size(); ++group) {
        batch_group_t const& named = groups[group];
        st::basic_vector<select_result_t> answers;
        if (answers.resize(named.keys.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A batch could not track what its keys answered"};

        status_t overflowed;
        auto sink = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            std::size_t const at = named.requests[request];
            if (lengths[at] == ustore_length_missing_k) return;
            if (offset + bytes.size() > lengths[at]) {
                overflowed = {status_code_t::input_output_k, "A value grew between its measurement and its read"};
                return;
            }
            if (!bytes.empty()) std::memcpy(tape.data() + starts[at] + offset, bytes.data(), bytes.size());
        };
        status_t const read = database.store->select(any_collection_id_t {named.numeric_id},
                                                     std::span<stored_key_t const> {named.keys},
                                                     std::span<select_result_t> {answers}, sink);
        if (read.failed()) return read;
        if (overflowed.failed()) return overflowed;
    }
    return {};
}

/** Reads every place of @p places through @p transaction into @p tape, one key at a time. */
inline status_t read_through_transaction(opened_database_t const& database, opened_transaction_t const& transaction,
                                         watch_schedule_t schedule, places_arg_t const& places,
                                         std::span<ustore_length_t const> starts,
                                         std::span<ustore_length_t const> lengths, std::span<byte_t> tape) noexcept {
    for (std::size_t index = 0; index != places.size(); ++index) {
        if (lengths[index] == ustore_length_missing_k) continue;
        place_t const place = places[index];
        stored_value_size_t size = 0;
        std::span<byte_t> const into = tape.subspan(starts[index], lengths[index]);
        if (status_t const read = read_through(*transaction.held, schedule, numbered(database, place.collection),
                                               place.key, into, size);
            read.failed())
            return read;
    }
    return {};
}

#pragma endregion Reads

#pragma region Writes

/** @p content as the store spells the bytes of one value. */
[[nodiscard]] inline stored_value_view_t bytes_of(value_view_t content) noexcept {
    return stored_value_view_t {reinterpret_cast<byte_t const*>(content.data()), content.size()};
}

/** Stages one place into @p handler, which a missing value erases. */
inline status_t stage_one(opened_database_t const& database, store_transaction_t& handler, watch_schedule_t schedule,
                          place_t place, value_view_t content) noexcept {
    collection_numeric_id_t const numeric_id = numbered(database, place.collection);
    if (content) return write_through(handler, schedule, numeric_id, place.key, bytes_of(content));
    return erase_through(handler, schedule, numeric_id, place.key);
}

/**
 *  @brief Commits @p places and @p contents as a transaction of its own, watching nothing.
 *
 *  A write that bypassed the registry would land where no watcher could see it, so a batch outside a
 *  caller's transaction takes one of its own; its buffer is sized to the batch rather than to the
 *  bound a caller's transaction takes, since it lives for exactly one call.
 */
inline status_t commit_batch(opened_database_t& database, places_arg_t const& places,
                             contents_arg_t const& contents) noexcept {
    std::size_t bytes = 0;
    for (std::size_t index = 0; index != contents.size(); ++index) bytes += contents[index].size();
    transaction_config_t shape;
    shape.write_buffer_max_bytes = std::max<std::size_t>(bytes, 1);
    shape.write_buffer_max_elements = std::max<std::size_t>(places.size(), 1);

    transaction_id_t id = invalid_transaction_id_k;
    do { id = uuid_to_u64(uuid_generator()); } while (database.store->has_transaction(id));
    auto [opened, create_transaction_status] = database.store->create_transaction(id, shape);
    if (create_transaction_status.failed()) return create_transaction_status;

    for (std::size_t index = 0; index != places.size(); ++index)
        if (status_t const staged = stage_one(database, *opened, watch_schedule_t::never_k, places[index],
                                              contents[index]);
            staged.failed())
            return staged;
    return opened->commit();
}

#pragma endregion Writes

#pragma region Catalog

/** Creates the collection @p name, or answers the number it already had. */
inline expected<collection_numeric_id_t> reserve_collection(store_t& store, std::string_view name,
                                                            collection_config_t const& config) noexcept {
    any_collection_id_t const id {collection_alphanumeric_id_t {string_view_t {name}}};
    collection_numeric_id_t const known = store.collection_numeric_id(id);
    if (known != collection_invalid_numeric_id_k) return known;
    return store.create_collection(string_view_t {name}, config);
}

/**
 *  @brief Removes every key of @p numeric_id, or empties every value when @p mode asks for that.
 *
 *  The keys are collected under a snapshot that is let go before the first write, since a publication
 *  waits on every hold, and they are written back in windows the collection's write buffer admits.
 */
inline status_t empty_collection(opened_database_t& database, collection_numeric_id_t numeric_id,
                                 ustore_drop_mode_t mode) noexcept {
    collection_config_t shape;
    if (status_t const known = database.store->collection_config(numeric_id, shape); known.failed()) return known;

    st::basic_vector<stored_key_t> keys;
    {
        auto [taken, take_snapshot_status] = database.store->take_snapshot();
        if (take_snapshot_status.failed()) return take_snapshot_status;
        if (status_t const walked = collect_keys(taken, numeric_id, limit<stored_key_t>::min(), keys); walked.failed())
            return walked;
    }
    if (keys.empty()) return {};

    st::basic_vector<stored_value_size_t> sizes;
    if (mode == ustore_drop_vals_k && sizes.resize(keys.size()) != st::status_t::success_k)
        return {status_code_t::out_of_memory_k, "A drop could not track the values it empties"};
    for (std::size_t index = 0; index != sizes.size(); ++index) sizes[index] = 0;

    // A window is half the buffer, so one flush always leaves room for the whole of the next one.
    std::size_t const window = std::max<std::size_t>(shape.write_buffer_max_elements / 2, 1);
    any_collection_id_t const id {numeric_id};
    for (std::size_t first = 0; first != keys.size(); first += std::min(window, keys.size() - first)) {
        std::size_t const count = std::min(window, keys.size() - first);
        std::span<stored_key_t const> const named {keys.data() + first, count};
        if (mode == ustore_drop_keys_vals_k) {
            if (status_t const removed = database.store->remove(id, named); removed.failed()) return removed;
            continue;
        }
        std::span<stored_value_size_t const> const emptied {sizes.data() + first, count};
        if (status_t const written = database.store->insert(id, named, std::span<byte_t const> {}, emptied);
            written.failed())
            return written;
    }
    return {};
}

/** Drops every collection the catalog names beyond the two the engine reserves. */
inline status_t drop_named_collections(opened_database_t& database) noexcept {
    st::basic_vector<collection_ids_t> const listed = database.store->list_collections();
    status_t first;
    for (std::size_t index = 0; index != listed.size(); ++index) {
        if (reserved(database, listed[index].numeric_id)) continue;
        if (status_t const dropped = database.store->drop_collection(any_collection_id_t {listed[index].numeric_id});
            dropped.failed() && first.succeeded())
            first = dropped;
    }
    return first;
}

#pragma endregion Catalog

#pragma region Lifetime

/** The resources @p arguments name, with a data directory and a capacity where they named none. */
inline status_t resolve_resources(ustore_database_init_nvme_t const& arguments,
                                  resources_config_t& resources) noexcept {
    if (!arguments.directory || arguments.directory[0] == '\0')
        return {status_code_t::invalid_argument_k, "A database needs a directory"};
    if (!resources.directory.try_assign(string_view_t {arguments.directory}))
        return {status_code_t::out_of_memory_k, "No room for the directory of the database"};
    if (arguments.data_directories_count && !arguments.data_directories)
        return {status_code_t::invalid_argument_k, "Counted data directories are null"};

    resources.data_directories.clear();
    for (ustore_data_directory_t const& given :
         std::span {arguments.data_directories, static_cast<std::size_t>(arguments.data_directories_count)}) {
        if (!given.path || given.path[0] == '\0')
            return {status_code_t::invalid_argument_k, "A data directory needs a path"};
        data_directory_config_t directory;
        if (!directory.path.try_assign(string_view_t {given.path}))
            return {status_code_t::out_of_memory_k, "No room for a data directory of the database"};
        directory.max_size = given.max_bytes ? given.max_bytes : default_volume_bytes_k;
        if (resources.data_directories.push_back(std::move(directory)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for another data directory"};
    }
    if (!resources.data_directories.empty()) return {};

    char path[path_max_bytes_k];
    if (status_t const joined = join_path(resources.directory.c_str(), data_directory_name_k, path); joined.failed())
        return joined;
    data_directory_config_t only;
    if (!only.path.try_assign(string_view_t {path}))
        return {status_code_t::out_of_memory_k, "No room for the data directory of the database"};
    only.max_size = default_volume_bytes_k;
    if (resources.data_directories.push_back(std::move(only)) != st::status_t::success_k)
        return {status_code_t::out_of_memory_k, "No room for the data directory of the database"};
    return {};
}

/** Replaces @p value with @p requested unless it is zero, answering whether @p requested fits @p value. */
template <typename value_type_>
[[nodiscard]] bool take_unless_zero(value_type_& value, ustore_size_t requested) noexcept {
    if (requested == 0) return true;
    if (!std::in_range<value_type_>(requested)) return false;
    value = static_cast<value_type_>(requested);
    return true;
}

/** Reads the engine's own options from @p arguments, keeping the engine's default for every zero among them. */
inline status_t read_engine_options(ustore_database_init_nvme_t const& arguments, resources_config_t& resources,
                                    database_config_t& transactions, user_collection_config_t& collections) noexcept {
    switch (arguments.transport) {
    case ustore_transport_automatic_k: resources.io_transport = io_transport_t::automatic_k; break;
    case ustore_transport_blocking_k: resources.io_transport = io_transport_t::blocking_k; break;
    case ustore_transport_linux_uring_k: resources.io_transport = io_transport_t::linux_uring_k; break;
    case ustore_transport_linux_aio_k: resources.io_transport = io_transport_t::linux_aio_k; break;
    case ustore_transport_linux_spdk_k: resources.io_transport = io_transport_t::linux_spdk_k; break;
    default: return {status_code_t::invalid_argument_k, "Unknown transport"};
    }

    if (arguments.gpu_devices_count && !arguments.gpu_devices)
        return {status_code_t::invalid_argument_k, "Counted GPU devices are null"};
    for (int const device : std::span {arguments.gpu_devices, static_cast<std::size_t>(arguments.gpu_devices_count)})
        if (resources.gpu_devices.emplace_back(device) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room for another GPU device"};

    // A fixed value size leaves the largest one unset unless it was asked for, as exactly one of the two may be set
    if (arguments.collection_value_size) collections.value_max_size = 0;
    bool const fits =
        take_unless_zero(resources.memory_limit, arguments.memory_limit) &&
        take_unless_zero(resources.threads, arguments.threads) &&
        take_unless_zero(resources.gpu_memory_limit, arguments.gpu_memory_limit) &&
        take_unless_zero(resources.shared_io_channels, arguments.shared_io_channels) &&
        take_unless_zero(resources.channels.depth, arguments.queue_depth) &&
        take_unless_zero(resources.channels.request_buffer_bytes, arguments.request_buffer_bytes) &&
        take_unless_zero(resources.channels.request_buffers_count, arguments.request_buffers_count) &&
        take_unless_zero(resources.channels.stream_buffer_bytes, arguments.stream_buffer_bytes) &&
        take_unless_zero(resources.channels.stream_buffers_count, arguments.stream_buffers_count) &&
        take_unless_zero(transactions.default_transaction.write_buffer_max_bytes, arguments.transaction_max_bytes) &&
        take_unless_zero(transactions.default_transaction.write_buffer_max_elements,
                         arguments.transaction_max_elements) &&
        take_unless_zero(collections.concurrency_limit, arguments.collection_concurrency_limit) &&
        take_unless_zero(collections.cache_limit, arguments.collection_cache_limit) &&
        take_unless_zero(collections.write_buffer_max_bytes, arguments.collection_write_buffer_max_bytes) &&
        take_unless_zero(collections.write_buffer_max_elements, arguments.collection_write_buffer_max_elements) &&
        take_unless_zero(collections.first_level_max_bytes, arguments.collection_first_level_max_bytes) &&
        take_unless_zero(collections.level_enlarge_factor, arguments.collection_level_enlarge_factor) &&
        take_unless_zero(collections.value_size, arguments.collection_value_size) &&
        take_unless_zero(collections.value_max_size, arguments.collection_value_max_size);
    if (!fits) return {status_code_t::invalid_argument_k, "An option of the engine exceeds its range"};
    return {};
}

/** Checks @p resources and @p collections, whose issue messages live only inside this call. */
inline status_t validated(resources_config_t const& resources, database_config_t const& transactions,
                          user_collection_config_t const& collections) noexcept {
    auto ignore_issue = [](issue_kind_t, std::string_view) noexcept {};
    if (!validate(resources, transactions, ignore_issue) || !validate(resources, collections, ignore_issue))
        return {status_code_t::invalid_argument_k, "The configuration of the engine is invalid"};
    return {};
}

/** Mounts @p database over the storage its resources opened, which must be the one on files. */
inline status_t mount_store(opened_database_t& database, database_config_t const& transactions) noexcept {
    return database.resources->storage.visit([&](auto& mount) noexcept -> status_t {
        using mount_t = std::remove_reference_t<decltype(mount)>;
        if constexpr (std::is_same_v<mount_t, storage_mount_t>)
            return database.store->mount(mount.router(), mount.channels(), transactions);
        else return {status_code_t::unsupported_k, "The engine mounts its volumes on files alone"};
    });
}

/** Opens the database @p arguments describe, with its two reserved collections. */
inline expected<opened_database_t*> open_database(ustore_database_init_nvme_t const& arguments) noexcept {
    resources_config_t resources;
    resources.shared_io_channels = engine_shared_io_channels_k;
    database_config_t transactions = engine_database_defaults();
    user_collection_config_t collections = engine_collection_defaults();

    if (status_t const resolved = resolve_resources(arguments, resources); resolved.failed()) return resolved;
    if (status_t const read = read_engine_options(arguments, resources, transactions, collections); read.failed())
        return read;
    if (status_t const checked = validated(resources, transactions, collections); checked.failed()) return checked;

    std::unique_ptr<opened_database_t> opened {new (std::nothrow) opened_database_t {}};
    if (!opened) return status_t {status_code_t::out_of_memory_k, "No room for the database"};
    opened->engine = &engine_k;
    opened->collection_defaults = collections;

    auto [budget, open_resources_status] = open_resources(resources, uuid_t {});
    if (open_resources_status.failed()) return open_resources_status;
    opened->resources = std::move(budget);

    opened->store = std::unique_ptr<store_t> {new (std::nothrow) store_t {opened->resources}};
    if (!opened->store) return status_t {status_code_t::out_of_memory_k, "No room for the store"};
    if (status_t const mounted = mount_store(*opened, transactions); mounted.failed()) return mounted;

    collection_config_t const shape = create_collection_config(collections);
    auto [main, reserve_main_status] = reserve_collection(*opened->store, main_collection_name_k, shape);
    if (reserve_main_status.failed()) return reserve_main_status;
    auto [names, reserve_names_status] = reserve_collection(*opened->store, names_collection_name_k, shape);
    if (reserve_names_status.failed()) return reserve_names_status;
    opened->main = main;
    opened->names = names;
    return opened.release();
}

/** Unmounts the store of @p database while its mount is still there, then lets both go. */
inline void close_database(opened_database_t& database) noexcept {
    if (database.store) { [[maybe_unused]] status_t const unmounted = database.store->unmount(); }
    database.store.reset();
    database.resources.reset();
}

#pragma endregion Lifetime

#pragma region C ABI

void database_init(ustore_database_init_nvme_t& arguments) noexcept {
    if (!arguments.db) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the database"};
        return;
    }
    auto [opened, open_database_status] = open_database(arguments);
    if (open_database_status.failed()) {
        *arguments.error = open_database_status;
        return;
    }
    *arguments.db = static_cast<handle_t*>(opened);
}

void database_capabilities(ustore_database_capabilities_t& arguments) noexcept {
    if (!arguments.db || !arguments.capabilities) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need a database and an output for the capabilities"};
        return;
    }
    *arguments.capabilities = ustore_capabilities_t(ustore_supports_transactions_k | ustore_supports_collections_k);
}

void read(ustore_read_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "The database is uninitialized"};
        return;
    }
    places_arg_t const places = places_of(arguments);
    validate_read(arguments.transaction, places, arguments.options, arguments.error);
    if (arguments.error->status) return;
    if (arguments.snapshot) {
        *arguments.error = {ustore_error_unsupported_k, "This engine keeps no snapshots"};
        return;
    }

    opened_database_t& database = database_of(arguments.db);
    opened_transaction_t* const transaction = transaction_of(arguments.transaction);
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    range_or_dummy<bits_span_t> presences = arena.alloc_or_dummy(places.count, arguments.error, arguments.presences);
    range_or_dummy<std::span<ustore_length_t>> offsets = arena.alloc_or_dummy(places.count + 1, arguments.error,
                                                                              arguments.offsets);
    range_or_dummy<std::span<ustore_length_t>> lengths = arena.alloc_or_dummy(places.count, arguments.error,
                                                                              arguments.lengths);
    if (arguments.error->status) return;

    // The columns are measured first, so the tape is laid out once and every read lands where it belongs.
    std::span<ustore_length_t> const measured = arena.alloc<ustore_length_t>(places.count, arguments.error);
    std::span<ustore_length_t> const starts = arena.alloc<ustore_length_t>(places.count, arguments.error);
    if (arguments.error->status) return;

    std::size_t used = 0;
    for (std::size_t index = 0; index != places.size(); ++index) {
        place_t const place = places[index];
        measured[index] = measured_length(database, transaction, numbered(database, place.collection), place.key);
        starts[index] = static_cast<ustore_length_t>(used);
        presences[index] = measured[index] != ustore_length_missing_k;
        offsets[index] = static_cast<ustore_length_t>(used);
        lengths[index] = measured[index];
        if (measured[index] != ustore_length_missing_k) used += measured[index];
    }
    offsets[places.count] = static_cast<ustore_length_t>(used);
    if (!arguments.values) return;

    std::span<byte_t> const tape = arena.alloc<byte_t>(used, arguments.error, buffer_alignment_k);
    if (arguments.error->status) return;
    *arguments.values = reinterpret_cast<ustore_bytes_ptr_t>(tape.data());
    if (used == 0) return;

    std::span<ustore_length_t const> const widths {measured};
    std::span<ustore_length_t const> const bases {starts};
    if (transaction) {
        status_t const read = read_through_transaction(database, *transaction, schedule_of(arguments.options), places,
                                                       bases, widths, tape);
        reported_failure(read, arguments.error);
        return;
    }

    st::basic_vector<batch_group_t> groups;
    if (reported_failure(group_places(database, places, groups), arguments.error)) return;
    reported_failure(read_batched(database, groups, bases, widths, tape), arguments.error);
}

void write(ustore_write_t& arguments) noexcept {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "The database is uninitialized"};
        return;
    }
    places_arg_t const places = places_of(arguments);
    contents_arg_t const contents = contents_of(arguments);
    validate_write(arguments.transaction, places, contents, arguments.options, arguments.error);
    if (arguments.error->status) return;

    opened_database_t& database = database_of(arguments.db);
    if (opened_transaction_t* const transaction = transaction_of(arguments.transaction)) {
        watch_schedule_t const schedule = schedule_of(arguments.options);
        for (std::size_t index = 0; index != places.size(); ++index)
            if (reported_failure(stage_one(database, *transaction->held, schedule, places[index], contents[index]),
                                 arguments.error))
                return;
        return;
    }

    if (arguments.tasks_count != 0)
        if (reported_failure(commit_batch(database, places, contents), arguments.error)) return;
    if (!(arguments.options & ustore_option_write_flush_k)) return;
    if (reported_failure(database.store->flush(), arguments.error)) return;
    reported_failure(database.store->compact(), arguments.error);
}

void scan(ustore_scan_t& arguments) noexcept {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "The database is uninitialized"};
        return;
    }
    scans_arg_t const scans = scans_of(arguments);
    validate_scan(arguments.transaction, scans, arguments.options, arguments.error);
    if (arguments.error->status || !arguments.tasks_count) return;
    if (!arguments.keys) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the keys"};
        return;
    }
    if (arguments.snapshot) {
        *arguments.error = {ustore_error_unsupported_k, "This engine keeps no snapshots"};
        return;
    }

    opened_database_t& database = database_of(arguments.db);
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    key_tape_t tape {arena, scans.size(), arguments.offsets, arguments.counts, arguments.error};
    tape.reserve(expected_keys(scans));
    if (arguments.error->status) return;

    auto [taken, take_snapshot_status] = database.store->take_snapshot();
    if (reported_failure(take_snapshot_status, arguments.error)) return;
    for (std::size_t task = 0; task != scans.size(); ++task) {
        scan_t const scan = scans[task];
        std::size_t const first = tape.size();
        status_t const walked = walk_collection(
            taken, numbered(database, scan.collection), scan.min_key,
            [&](stored_key_t key, select_result_t state, stored_value_size_t) noexcept -> walk_control_t {
                if (state != select_result_t::success_k) return walk_control_t::resume_k;
                if (tape.push(key) == walk_control_t::halt_k) return walk_control_t::halt_k;
                return tape.size() - first == scan.limit ? walk_control_t::halt_k : walk_control_t::resume_k;
            });
        if (reported_failure(walked, arguments.error) || arguments.error->status) return;
        tape.close_task(task, first);
    }
    tape.finish(arguments.keys);
}

void sample(ustore_sample_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "The database is uninitialized"};
        return;
    }
    if (!arguments.count_limits || !arguments.keys) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need count limits and an output for the keys"};
        return;
    }
    if (arguments.snapshot) {
        *arguments.error = {ustore_error_unsupported_k, "This engine keeps no snapshots"};
        return;
    }

    sample_args_t const samples = samples_of(arguments);
    opened_database_t& database = database_of(arguments.db);
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    key_tape_t tape {arena, samples.size(), arguments.offsets, arguments.counts, arguments.error};
    if (arguments.error->status) return;

    auto [taken, take_snapshot_status] = database.store->take_snapshot();
    if (reported_failure(take_snapshot_status, arguments.error)) return;
    auto const now = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    sample_generator_t generator {now ^ reinterpret_cast<std::uintptr_t>(&tape)};

    for (std::size_t task = 0; task != samples.size(); ++task) {
        sample_arg_t const sample = samples[task];
        std::size_t const first = tape.size();
        std::size_t seen = 0;
        status_t const walked = walk_collection(
            taken, numbered(database, sample.collection), limit<stored_key_t>::min(),
            [&](stored_key_t key, select_result_t state, stored_value_size_t) noexcept -> walk_control_t {
                if (state != select_result_t::success_k) return walk_control_t::resume_k;
                std::size_t const slot = seen < sample.limit ? seen : generator() % (seen + 1);
                ++seen;
                if (slot >= sample.limit) return walk_control_t::resume_k;
                if (slot == tape.size() - first) return tape.push(key);
                tape[first + slot] = key;
                return walk_control_t::resume_k;
            });
        if (reported_failure(walked, arguments.error) || arguments.error->status) return;
        tape.close_task(task, first);
    }
    tape.finish(arguments.keys);
}

void measure(ustore_measure_t& arguments) noexcept {
    if (!arguments.tasks_count) return;
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "The database is uninitialized"};
        return;
    }
    if (!enum_is_subset(arguments.options, ustore_option_dont_discard_memory_k)) {
        *arguments.error = {ustore_error_invalid_argument_k, "Invalid options"};
        return;
    }
    if (arguments.snapshot) {
        *arguments.error = {ustore_error_unsupported_k, "This engine keeps no snapshots"};
        return;
    }

    opened_database_t& database = database_of(arguments.db);
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;
    std::size_t const count = arguments.tasks_count;
    range_or_dummy<std::span<ustore_size_t>> min_cardinalities = arena.alloc_or_dummy(count, arguments.error,
                                                                                      arguments.min_cardinalities);
    range_or_dummy<std::span<ustore_size_t>> max_cardinalities = arena.alloc_or_dummy(count, arguments.error,
                                                                                      arguments.max_cardinalities);
    range_or_dummy<std::span<ustore_size_t>> min_value_bytes = arena.alloc_or_dummy(count, arguments.error,
                                                                                    arguments.min_value_bytes);
    range_or_dummy<std::span<ustore_size_t>> max_value_bytes = arena.alloc_or_dummy(count, arguments.error,
                                                                                    arguments.max_value_bytes);
    range_or_dummy<std::span<ustore_size_t>> min_space_usages = arena.alloc_or_dummy(count, arguments.error,
                                                                                     arguments.min_space_usages);
    range_or_dummy<std::span<ustore_size_t>> max_space_usages = arena.alloc_or_dummy(count, arguments.error,
                                                                                     arguments.max_space_usages);
    if (arguments.error->status) return;

    strided_iterator<ustore_collection_t const> const collections {arguments.collections, arguments.collections_stride};
    strided_iterator<ustore_key_t const> const start_keys {arguments.start_keys, arguments.start_keys_stride};
    strided_iterator<ustore_key_t const> const end_keys {arguments.end_keys, arguments.end_keys_stride};

    auto [taken, take_snapshot_status] = database.store->take_snapshot();
    if (reported_failure(take_snapshot_status, arguments.error)) return;
    for (std::size_t task = 0; task != count; ++task) {
        auto const index = static_cast<std::ptrdiff_t>(task);
        ustore_collection_t const collection = collections ? collections[index] : ustore_collection_main_k;
        stored_key_t const lower = start_keys ? start_keys[index] : limit<stored_key_t>::min();
        stored_key_t const last = end_keys ? end_keys[index] : limit<stored_key_t>::max();
        std::size_t cardinality = 0;
        std::size_t value_bytes = 0;
        status_t const walked = walk_collection(
            taken, numbered(database, collection), lower,
            [&](stored_key_t key, select_result_t state, stored_value_size_t size) noexcept -> walk_control_t {
                if (last < key) return walk_control_t::halt_k;
                if (state != select_result_t::success_k) return walk_control_t::resume_k;
                ++cardinality;
                value_bytes += size;
                return walk_control_t::resume_k;
            });
        if (reported_failure(walked, arguments.error)) return;

        min_cardinalities[task] = cardinality;
        max_cardinalities[task] = cardinality;
        min_value_bytes[task] = value_bytes;
        max_value_bytes[task] = value_bytes;
        min_space_usages[task] = value_bytes;
        max_space_usages[task] = std::numeric_limits<ustore_size_t>::max();
    }
}

void collection_list(ustore_collection_list_t& arguments) noexcept {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "The database is uninitialized"};
        return;
    }
    if (!arguments.count) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the count"};
        return;
    }

    opened_database_t& database = database_of(arguments.db);
    linked_memory_lock_t arena = linked_memory(arguments.arena, arguments.options, arguments.error);
    if (arguments.error->status) return;

    st::basic_vector<collection_ids_t> const listed = database.store->list_collections();
    std::size_t named = 0;
    for (std::size_t index = 0; index != listed.size(); ++index)
        if (!reserved(database, listed[index].numeric_id)) ++named;

    range_or_dummy<std::span<ustore_collection_t>> ids = arena.alloc_or_dummy(named, arguments.error, arguments.ids);
    if (arguments.error->status) return;
    std::size_t written = 0;
    for (std::size_t index = 0; index != listed.size(); ++index)
        if (!reserved(database, listed[index].numeric_id))
            ids[written++] = static_cast<ustore_collection_t>(listed[index].numeric_id);
    *arguments.count = static_cast<ustore_size_t>(named);
}

void collection_create(ustore_collection_create_t& arguments) noexcept {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "The database is uninitialized"};
        return;
    }
    if (!arguments.id) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the collection"};
        return;
    }

    opened_database_t& database = database_of(arguments.db);
    user_collection_config_t const& shape = database.collection_defaults;

    // A collection is addressed by its number, so this name never leaves the engine and only has to be free.
    char name[collection_name_max_bytes_k];
    for (std::size_t suffix = database.store->size();; ++suffix) {
        auto const written = fmt::format_to_n(name, sizeof(name) - 1, "collection_{}", suffix);
        *written.out = '\0';
        if (!database.store->has_collection(any_collection_id_t {collection_alphanumeric_id_t {name}})) break;
    }

    auto [created, create_collection_status] = database.store->create_collection(string_view_t {name},
                                                                                 create_collection_config(shape));
    if (reported_failure(create_collection_status, arguments.error)) return;
    *arguments.id = static_cast<ustore_collection_t>(created);
}

void collection_drop(ustore_collection_drop_t& arguments) noexcept {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "The database is uninitialized"};
        return;
    }
    if (arguments.mode != ustore_drop_keys_vals_handle_k && arguments.mode != ustore_drop_keys_vals_k &&
        arguments.mode != ustore_drop_vals_k) {
        *arguments.error = {ustore_error_invalid_argument_k, "Unknown drop mode"};
        return;
    }

    opened_database_t& database = database_of(arguments.db);
    collection_numeric_id_t const numeric_id = numbered(database, arguments.id);
    if (arguments.mode == ustore_drop_keys_vals_handle_k) {
        if (reserved(database, numeric_id)) {
            *arguments.error = {ustore_error_invalid_argument_k, "This collection can only be cleared"};
            return;
        }
        reported_failure(database.store->drop_collection(any_collection_id_t {numeric_id}), arguments.error);
        return;
    }
    reported_failure(empty_collection(database, numeric_id, arguments.mode), arguments.error);
}

void snapshot_list(ustore_snapshot_list_t& arguments) noexcept {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "The database is uninitialized"};
        return;
    }
    if (!arguments.count) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need an output for the count"};
        return;
    }
    *arguments.count = 0;
    if (arguments.ids) *arguments.ids = nullptr;
}

void snapshot_create(ustore_snapshot_create_t& arguments) noexcept {
    *arguments.error = {ustore_error_unsupported_k, "This engine keeps no snapshots"};
}

void snapshot_export(ustore_snapshot_export_t& arguments) noexcept {
    *arguments.error = {ustore_error_unsupported_k, "This engine keeps no snapshots"};
}

void snapshot_drop(ustore_snapshot_drop_t& arguments) noexcept {
    *arguments.error = {ustore_error_unsupported_k, "This engine keeps no snapshots"};
}

void transaction_init(ustore_transaction_init_t& arguments) noexcept {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "The database is uninitialized"};
        return;
    }
    validate_transaction_begin(arguments.transaction, arguments.options, arguments.error);
    if (arguments.error->status) return;

    opened_database_t& database = database_of(arguments.db);
    if (opened_transaction_t* const opened = transaction_of(*arguments.transaction)) {
        opened->held->rollback();
        return;
    }

    transaction_id_t id = invalid_transaction_id_k;
    do { id = uuid_to_u64(uuid_generator()); } while (database.store->has_transaction(id));
    auto [started, create_transaction_status] = database.store->create_transaction(id);
    if (reported_failure(create_transaction_status, arguments.error)) return;

    auto* const opened = new (std::nothrow) opened_transaction_t {{&engine_k}, &database, std::move(started)};
    if (!opened) {
        *arguments.error = {ustore_error_out_of_memory_k, "Failed to allocate the transaction"};
        return;
    }
    *arguments.transaction = static_cast<handle_t*>(opened);
}

void transaction_stage(ustore_transaction_stage_t& arguments) noexcept {
    if (!arguments.transaction) {
        *arguments.error = {ustore_error_uninitialized_k, "Transaction is uninitialized"};
        return;
    }
    opened_transaction_t& transaction = *transaction_of(arguments.transaction);
    if (reported_failure(transaction.held->stage(), arguments.error)) return;
    if (arguments.sequence_number) *arguments.sequence_number = transaction.held->last_stage_stamp();
}

void transaction_commit(ustore_transaction_commit_t& arguments) noexcept {
    validate_transaction_commit(arguments.transaction, arguments.options, arguments.error);
    if (arguments.error->status) return;

    opened_transaction_t& transaction = *transaction_of(arguments.transaction);
    if (reported_failure(transaction.held->commit(), arguments.error)) return;
    if (arguments.sequence_number) *arguments.sequence_number = transaction.held->last_commit_stamp();
    if (!(arguments.options & ustore_option_write_flush_k)) return;
    reported_failure(transaction.database->store->flush(), arguments.error);
}

void database_control(ustore_database_control_t& arguments) noexcept {
    if (!arguments.db) {
        *arguments.error = {ustore_error_uninitialized_k, "The database is uninitialized"};
        return;
    }
    if (!arguments.request || !arguments.response) {
        *arguments.error = {ustore_error_invalid_argument_k, "Need a request and an output for the response"};
        return;
    }

    opened_database_t& database = database_of(arguments.db);
    std::string_view const request {arguments.request};
    *arguments.response = nullptr;
    if (request == "info") {
        mounted_storage_t const& storage = database.resources->storage;
        if (storage.report().transport == io_transport_t::linux_aio_k)
            *arguments.response = R"({"engine":"nvme","transport":"linux-aio"})";
        else if (storage.report().transport != io_transport_t::linux_uring_k)
            *arguments.response = R"({"engine":"nvme","transport":"blocking"})";
        else
            *arguments.response = storage.pinning() == range_pinning_t::pinned_k
                                      ? R"({"engine":"nvme","transport":"linux-uring","buffers":"pinned"})"
                                      : R"({"engine":"nvme","transport":"linux-uring","buffers":"unpinned"})";
        return;
    }
    if (request == "flush") {
        reported_failure(database.store->flush(), arguments.error);
        return;
    }
    if (request == "compact") {
        if (reported_failure(database.store->flush(), arguments.error)) return;
        reported_failure(database.store->compact(), arguments.error);
        return;
    }
    if (request == "clear") {
        if (reported_failure(empty_collection(database, database.main, ustore_drop_keys_vals_k), arguments.error))
            return;
        reported_failure(empty_collection(database, database.names, ustore_drop_keys_vals_k), arguments.error);
        return;
    }
    if (request == "reset") {
        if (reported_failure(drop_named_collections(database), arguments.error)) return;
        if (reported_failure(empty_collection(database, database.main, ustore_drop_keys_vals_k), arguments.error))
            return;
        reported_failure(empty_collection(database, database.names, ustore_drop_keys_vals_k), arguments.error);
        return;
    }
    if (request == "usage") {
        linked_memory_lock_t arena = linked_memory(arguments.arena, ustore_options_default_k, arguments.error);
        if (arguments.error->status) return;
        constexpr std::size_t response_capacity_k = 96;
        std::span<char> const response = arena.alloc<char>(response_capacity_k, arguments.error);
        if (arguments.error->status) return;
        auto const written = fmt::format_to_n(response.data(), response_capacity_k - 1, R"({{"keys":{},"bytes":{}}})",
                                              database.store->population_count(), database.store->size_bytes());
        *written.out = '\0';
        *arguments.response = response.data();
        return;
    }
    *arguments.error = {ustore_error_invalid_argument_k,
                        "Unknown request: expected clear, reset, flush, compact, info or usage"};
}

void transaction_free(ustore_transaction_t c_transaction) noexcept { delete transaction_of(c_transaction); }

void database_free(ustore_database_t c_database) noexcept {
    std::unique_ptr<opened_database_t> const freed {&database_of(c_database)};
    close_database(*freed);
}

#pragma endregion C ABI

engine_t const engine_k {
    .database_capabilities = &database_capabilities,
    .database_control = &database_control,
    .snapshot_list = &snapshot_list,
    .snapshot_create = &snapshot_create,
    .snapshot_export = &snapshot_export,
    .snapshot_drop = &snapshot_drop,
    .read = &read,
    .write = &write,
    .scan = &scan,
    .sample = &sample,
    .measure = &measure,
    .collection_create = &with_names<ustore_collection_create_t, &collection_create>,
    .collection_drop = &with_names<ustore_collection_drop_t, &collection_drop>,
    .collection_list = &with_names<ustore_collection_list_t, &collection_list>,
    .collection_find = &collection_find,
    .strings_write = &strings_write,
    .strings_read = &strings_read,
    .strings_scan = &strings_scan,
    .transaction_init = &transaction_init,
    .transaction_stage = &transaction_stage,
    .transaction_commit = &transaction_commit,
    .transaction_free = &transaction_free,
    .database_free = &database_free,
};

} // namespace unum::ustore::nvme

using namespace unum::ustore;
using namespace unum::ustore::nvme;

void ustore_database_init_nvme(ustore_database_init_nvme_t* arguments) { database_init(*arguments); }
