/**
 *  @file include/ustore/blobs/nvme_tree.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The configurations and schemas of UStoreNVMe, the levels of one LSM tree, the merge of one level into
 *      the next, and the level set readers see.
 *
 *  Configurations and on-disk schemas are kept as JSON through yyjson and validated against @c storage_limits_t.
 *
 *  Level zero takes one run per flush of the memory runs, so its runs overlap and only their age tells which
 *  version of a key is current: a lookup walks them newest first and a scan merges them by smallest key.
 *  Every deeper level is written by a merge, so its runs are disjoint and ascending and a lookup is a
 *  binary search over their bounds. Both shapes carry remove deltas; only the bottom level may forget one,
 *  which is a property of the merge that writes it rather than of the level that holds it.
 *
 *  A level holds its runs through pointers that never move, so the level set a compaction publishes can
 *  name the same runs the set it replaces named. Ownership follows one rule and no other: a level frees
 *  its runs unless it has been told to @c relinquish them, which only a level set being retired is.
 *
 *  The merge itself is arithmetic over key slots and belongs to @c nvme_compaction.hpp, which decides who
 *  survives, cuts the survivors into runs that fit their bounds and plans where every value moves. What
 *  is left to the merger is the I/O: pointing the compactor at each source run's index, then writing the planned
 *  runs out of the plan it answered. Reads and writes ride separate channels because a channel is one queue pair
 *  with one completion queue, so a read pipeline polling on the channel a write pipeline submits on would swallow
 *  the writes' completions.
 *
 *  A flush writes one run and a compaction writes a whole level, and both publish by building the next
 *  level set beside the current one, which no one changes once a reader may hold it. The tree hands the
 *  new set to the publication hook its caller names, which publishes it to readers and returns only once
 *  no reader is left in the set it replaced; that set is freed then, which is the whole of the
 *  reclamation, and readers never wait on a writer. A batched read refills the ring as
 *  completions retire rather than submitting a batch and waiting on it, so the depth a read reaches is the
 *  queue pair's and not the batch's.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::int64_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcmp`, `std::memcpy`, `std::memmove`, `std::memset`

#include <algorithm>   // `std::clamp`, `std::max`, `std::min`
#include <atomic>      // `std::memory_order_acquire`, `std::memory_order_release`
#include <concepts>    // `std::invocable`, `std::same_as`, `std::unsigned_integral`
#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::exchange`, `std::forward`, `std::move`

#include <fmt/format.h>
#include <yyjson.h>

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/blobs/nvme_compaction.hpp"   // `host_compactor_t`, `compaction_output`, `compaction_kind_t`
#include "ustore/blobs/nvme_key_slot.hpp"     // `storage_limits_t`, `stored_value_size_t`, `value_layout_t`
#include "ustore/blobs/nvme_sorted_run.hpp"   // `lsm_sorted_run`, `sorted_run_header_t`, `sorted_run_layout`
#include "ustore/utilities/nvme_channels.hpp" // `io_channel_t`, `io_channel_config_t`, `basic_read_pipeline`
#include "ustore/utilities/nvme_ring.hpp"     // `extent_t`, `uuid_t`, `uuid_generator`
#include "ustore/utilities/nvme_volume.hpp"   // `resources_config_t`, `blob_kind_t`
#include "ustore/utilities/types.hpp"         // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::blobs {

using utilities::cancellation_state_t;
using utilities::compaction_state_t;
using utilities::fields_storage_requirements;
using utilities::load;
using utilities::load_fields;
using utilities::pooled_buffer_t;
using utilities::readers_count_t;
using utilities::resources_config_t;
using utilities::ring_concept;
using utilities::save;
using utilities::save_fields;
using utilities::serializable_concept;
using utilities::storage_requirements;
using utilities::string_t;
using utilities::string_view_t;
using utilities::time_point_t;
using utilities::uuid_from_text;
using utilities::uuid_generator;
using utilities::uuid_text_length_k;
using utilities::uuid_to_text;

#pragma region Names

/** The name of one collection. */
using collection_alphanumeric_id_t = string_t;

/** The name and the ID of one collection. */
struct collection_ids_t {

    /** The name users address the collection by. */
    collection_alphanumeric_id_t name;

    /** The UUID its schema, every commit and the C ABI carry. */
    collection_id_t id;
};

/** The canonical text of @p identifier, as a schema document carries it. */
inline expected<string_t> uuid_to_string(uuid_t const& identifier) noexcept {
    char text[uuid_t::printed_length_k] = {};
    identifier.print(text);
    string_t printed;
    if (sz::failed(printed.assign(string_view_t {text, uuid_t::printed_length_k})))
        return status_t {status_code_t::out_of_memory_k, "No room for the text of an identifier"};
    return printed;
}

#pragma endregion Names

#pragma region Configurations and Schemas

/** Version every configuration and schema document carries; any other version is rejected. */
inline constexpr char const format_version_k[] = "1.0";

#pragma region Configurations

/** Limits of one collection, which the @c collection_* fields of the NVMe init call set. */
struct user_collection_config_t {
    std::size_t concurrency_limit = 32;
    std::size_t cache_limit = 100 * 1024 * 1024ull;
    std::size_t memory_runs_max_deltas = 100'000;
    std::size_t memory_runs_max_bytes = 100 * 1024 * 1024ull;
    std::size_t level_enlarge_factor = 4;
    stored_value_size_t value_size = 0;
    stored_value_size_t value_max_size = 4 * 1024;
};

/** Value sizes of one sorted run file. */
struct sorted_run_config_t {
    stored_value_size_t value_size = 0;
    stored_value_size_t value_max_size = 0;
};

/** Configuration of one LSM level. */
struct lsm_level_config_t {
    sorted_run_config_t sorted_run;
};

/** Configuration of one LSM tree, whose deeper levels grow from the first. */
struct lsm_tree_config_t {
    lsm_level_config_t lsm_level_0;
    std::size_t level_enlarge_factor = 0;
};

/** Configuration of one collection. */
struct collection_config_t {
    lsm_tree_config_t lsm_tree;
    std::size_t concurrency_limit = 0;
    std::size_t cache_limit = 0;
    std::size_t memory_runs_max_bytes = 0;
    std::size_t memory_runs_max_deltas = 0;
};

/** Delta limits of one transaction, which the @c transaction_max_* fields of the NVMe init call set. */
struct transaction_config_t {
    std::size_t max_bytes = 0;
    std::size_t max_deltas = 0;
};

/** Configuration of one single-collection database: a collection with transactions of its own. */
struct single_collection_database_config_t {
    collection_config_t collection;
    transaction_config_t default_transaction;
};

/** Configuration of one database: collections sharing transactions. */
struct database_config_t {
    transaction_config_t default_transaction {100 * 1024 * 1024ull, 100'000};
};

/** Limits of one single-collection database: its collection and its transactions. */
struct user_single_collection_database_config_t {
    user_collection_config_t collection;
    transaction_config_t default_transaction {100 * 1024 * 1024ull, 100'000};
};

/** Expands the limits of one collection into the configuration of its LSM tree. */
inline collection_config_t create_collection_config(user_collection_config_t const& user_config) noexcept {
    collection_config_t config;
    config.concurrency_limit = user_config.concurrency_limit;
    config.cache_limit = user_config.cache_limit;
    config.memory_runs_max_bytes = user_config.memory_runs_max_bytes;
    config.memory_runs_max_deltas = user_config.memory_runs_max_deltas;
    config.lsm_tree.level_enlarge_factor = user_config.level_enlarge_factor;
    config.lsm_tree.lsm_level_0.sorted_run.value_size = user_config.value_size;
    config.lsm_tree.lsm_level_0.sorted_run.value_max_size = user_config.value_max_size;
    return config;
}

/** Expands the limits of one single-collection database into its configuration. */
inline single_collection_database_config_t create_single_collection_database_config(
    user_single_collection_database_config_t const& user_config) noexcept {
    return {create_collection_config(user_config.collection), user_config.default_transaction};
}

#pragma endregion Configurations

#pragma region Schemas

/** Whether a schema describes a live database or its backup. */
enum class schema_kind_t : std::uint8_t {
    unknown_k = 0x00,
    database_k = 0x01,
    backup_k = 0x02,
};

/** What a schema document describes, read without parsing the schema itself. */
struct schema_header_t {
    schema_kind_t schema_kind = schema_kind_t::unknown_k;
    database_kind_t database_kind = database_kind_t::unknown_k;
};

/** One sorted run file of an LSM level. */
struct sorted_run_schema_t {
    string_t file_name;
};

/** The sorted run files of one LSM level. */
struct lsm_level_schema_t {
    using sorted_runs_t = st::basic_vector<sorted_run_schema_t>;

    sorted_runs_t sorted_runs;
};

/** The levels of one LSM tree. */
struct lsm_tree_schema_t {
    using lsm_levels_t = st::basic_vector<lsm_level_schema_t>;

    lsm_levels_t lsm_levels;
};

/** The files of one collection. */
struct collection_schema_t {
    lsm_tree_schema_t lsm_tree;
};

/** The files a saved snapshot of one collection shares with its source; saved snapshots are not yet written. */
struct collection_snapshot_schema_t {
    lsm_tree_schema_t lsm_tree;
    collection_id_t source_id;
};

/** The files and snapshots of one single-collection database. */
struct single_collection_database_schema_t {
    static constexpr schema_kind_t schema_kind_k = schema_kind_t::database_k;
    static constexpr database_kind_t database_kind_k = database_kind_t::single_collection_database_k;

    /** One saved snapshot of the single-collection database's collection, which nothing writes yet. */
    struct snapshot_t {
        collection_snapshot_schema_t collection;
        snapshot_id_t id = invalid_snapshot_id_k;
        time_point_t time_stamp = time_point_t::max();
    };
    using snapshots_t = st::basic_vector<snapshot_t>;

    single_collection_database_id_t id;
    collection_schema_t collection;
    snapshots_t snapshots;
};

/** The collections and snapshots of one database. */
struct database_schema_t {
    static constexpr schema_kind_t schema_kind_k = schema_kind_t::database_k;
    static constexpr database_kind_t database_kind_k = database_kind_t::database_k;

    /** One collection of the database. */
    struct collection_t {
        collection_config_t config;
        collection_schema_t schema;
        string_t name;
        collection_id_t id;
        value_layout_t type = value_layout_t::unknown_k;
    };

    /** One saved snapshot across the database's collections, which nothing writes yet. */
    struct snapshot_t {

        /** One collection within the saved snapshot. */
        struct collection_t {
            collection_config_t source_config;
            string_t source_name;
            value_layout_t source_type = value_layout_t::unknown_k;
            snapshot_id_t id = invalid_snapshot_id_k;
            collection_snapshot_schema_t collection;
        };
        using collection_snapshots_schema_t = st::basic_vector<collection_t>;

        snapshot_id_t id = invalid_snapshot_id_k;
        time_point_t time_stamp = time_point_t::max();
        collection_snapshots_schema_t collections;
    };

    using collections_t = st::basic_vector<collection_t>;
    using snapshots_t = st::basic_vector<snapshot_t>;

    database_id_t id;
    collections_t collections;
    snapshots_t snapshots;
};

/** A schema saved as a document of its own. */
template <typename schema_>
concept database_schema_concept = std::same_as<schema_, single_collection_database_schema_t> ||
                                  std::same_as<schema_, database_schema_t>;

/** A schema saved as bytes behind a signed and versioned header, as backups are. */
template <typename schema_>
concept binary_schema_concept = serializable_concept<schema_> && requires {
    { schema_::schema_kind_k } -> std::convertible_to<schema_kind_t>;
    { schema_::database_kind_k } -> std::convertible_to<database_kind_t>;
};

/** The 12 bytes every binary schema file opens with. */
struct binary_schema_header_t {
    std::uint8_t signature[8] = {0x50, 0x47, 0xF5, 0xCD, 0xA9, 0x3B, 0xCC, 0xEF};
    schema_kind_t schema_kind = schema_kind_t::unknown_k;
    database_kind_t database_kind = database_kind_t::unknown_k;
    std::uint8_t major_version = 0;
    std::uint8_t minor_version = 1;
};

static_assert(sizeof(binary_schema_header_t) == 12);

/** The bytes of @p schema behind its header. */
template <binary_schema_concept schema_>
std::size_t binary_schema_storage_requirements(schema_ const& schema) noexcept {
    return sizeof(binary_schema_header_t) + schema.storage_requirements();
}

/** Saves the header of @p schema, then the schema itself. */
template <binary_schema_concept schema_>
status_t save_binary_schema(schema_ const& schema, bytes_span_t destination) noexcept {
    binary_schema_header_t header;
    header.schema_kind = schema_::schema_kind_k;
    header.database_kind = schema_::database_kind_k;
    return save_fields(destination, header, schema);
}

/** Loads a schema behind its header, rejecting another signature, kind or version. */
template <binary_schema_concept schema_>
status_t load_binary_schema(schema_& schema, bytes_view_t source) noexcept {
    binary_schema_header_t header;
    if (status_t const status = load(header, source); status.failed()) return status;
    binary_schema_header_t const current;
    if (std::memcmp(header.signature, current.signature, sizeof(current.signature)) != 0)
        return status_t {status_code_t::malformed_k, "Invalid signature"};
    if (header.schema_kind != schema_::schema_kind_k || header.database_kind != schema_::database_kind_k)
        return status_t {status_code_t::malformed_k, "Invalid schema"};
    if (header.major_version != current.major_version || header.minor_version != current.minor_version)
        return status_t {status_code_t::unsupported_k, "Version not supported"};
    return schema.load(source.subspan(sizeof(header)));
}

#pragma endregion Schemas

#pragma region Reading

/** The text of a JSON string, or an empty view for any other value. */
inline std::string_view json_string_view(yyjson_val* value) noexcept {
    return yyjson_is_str(value) ? std::string_view {yyjson_get_str(value), yyjson_get_len(value)} : std::string_view {};
}

/** Reads an unsigned number that fits @p value_. */
template <std::unsigned_integral value_>
[[nodiscard]] bool read_json(yyjson_val* value, value_& number) noexcept {
    if (!yyjson_is_uint(value) || yyjson_get_uint(value) > std::numeric_limits<value_>::max()) return false;
    number = static_cast<value_>(yyjson_get_uint(value));
    return true;
}

/** Reads a string. */
[[nodiscard]] inline bool read_json(yyjson_val* value, string_t& text) noexcept {
    if (!yyjson_is_str(value)) return false;
    return sz::succeeded(text.assign(string_view_t {yyjson_get_str(value), yyjson_get_len(value)}));
}

/** Reads a UUID from its printed form. */
[[nodiscard]] inline bool read_json(yyjson_val* value, uuid_t& id) noexcept {
    std::string_view const text = json_string_view(value);
    if (text.size() != uuid_t::printed_length_k) return false;
    std::span<char const, uuid_t::printed_length_k> const printed {text.data(), uuid_t::printed_length_k};
    if (!uuid_t::parses(printed)) return false;
    id = uuid_t::parse(printed);
    return true;
}

/** Reads a collection ID from its canonical text. */
[[nodiscard]] inline bool read_json(yyjson_val* value, collection_id_t& id) noexcept {
    auto [uuid, uuid_status] = uuid_from_text(json_string_view(value));
    id.uuid = uuid;
    return uuid_status.succeeded();
}

/** Reads a time point from its count of clock ticks since the epoch. */
[[nodiscard]] inline bool read_json(yyjson_val* value, time_point_t& time) noexcept {
    std::int64_t ticks = 0;
    if (yyjson_is_sint(value)) ticks = yyjson_get_sint(value);
    else if (yyjson_is_uint(value) && yyjson_get_uint(value) <= std::uint64_t(std::numeric_limits<std::int64_t>::max()))
        ticks = static_cast<std::int64_t>(yyjson_get_uint(value));
    else return false;
    time = time_point_t(typename time_point_t::duration(ticks));
    return true;
}

/** Reads a value layout from its numeric code. */
[[nodiscard]] inline bool read_json(yyjson_val* value, value_layout_t& value_layout) noexcept {
    std::uint16_t code = 0;
    if (!read_json(value, code)) return false;
    value_layout = static_cast<value_layout_t>(code);
    return true;
}

/** Reads an array, replacing the elements of @p elements. */
template <typename element_, template <typename> typename allocator_>
[[nodiscard]] bool read_json(yyjson_val* array, st::basic_vector<element_, allocator_<element_>>& elements) noexcept {
    if (!yyjson_is_arr(array)) return false;
    elements.clear();
    if (elements.reserve(yyjson_arr_size(array)) != st::status_t::success_k) return false;
    yyjson_arr_iter iterator;
    yyjson_arr_iter_init(array, &iterator);
    while (yyjson_val* const item = yyjson_arr_iter_next(&iterator)) {
        element_ element;
        if (!read_json(item, element)) return false;
        if (elements.push_back(std::move(element)) != st::status_t::success_k) return false;
    }
    return true;
}

/** Checks the @c version member of a document against @c format_version_k. */
inline status_t read_json_version(yyjson_val* root) noexcept {
    yyjson_val* const version = yyjson_obj_get(root, "version");
    if (!yyjson_is_str(version)) return status_t {status_code_t::malformed_k, "Invalid version format"};
    if (json_string_view(version) != format_version_k)
        return status_t {status_code_t::unsupported_k, "Version not supported"};
    return status_t {};
}

[[nodiscard]] inline bool read_json(yyjson_val* object, sorted_run_config_t& config) noexcept {
    return read_json(yyjson_obj_get(object, "value_size"), config.value_size) &&
           read_json(yyjson_obj_get(object, "value_max_size"), config.value_max_size);
}

[[nodiscard]] inline bool read_json(yyjson_val* object, lsm_level_config_t& config) noexcept {
    return read_json(yyjson_obj_get(object, "sorted_run"), config.sorted_run);
}

[[nodiscard]] inline bool read_json(yyjson_val* object, lsm_tree_config_t& config) noexcept {
    return read_json(yyjson_obj_get(object, "lsm_level_0"), config.lsm_level_0) &&
           read_json(yyjson_obj_get(object, "level_enlarge_factor"), config.level_enlarge_factor);
}

[[nodiscard]] inline bool read_json(yyjson_val* object, collection_config_t& config) noexcept {
    return read_json(yyjson_obj_get(object, "lsm_tree"), config.lsm_tree) &&
           read_json(yyjson_obj_get(object, "concurrency_limit"), config.concurrency_limit) &&
           read_json(yyjson_obj_get(object, "cache_limit"), config.cache_limit) &&
           read_json(yyjson_obj_get(object, "memory_runs_max_bytes"), config.memory_runs_max_bytes) &&
           read_json(yyjson_obj_get(object, "memory_runs_max_deltas"), config.memory_runs_max_deltas);
}

[[nodiscard]] inline bool read_json(yyjson_val* object, sorted_run_schema_t& schema) noexcept {
    return read_json(yyjson_obj_get(object, "file_name"), schema.file_name);
}

[[nodiscard]] inline bool read_json(yyjson_val* object, lsm_level_schema_t& schema) noexcept {
    return read_json(yyjson_obj_get(object, "sorted_runs"), schema.sorted_runs);
}

[[nodiscard]] inline bool read_json(yyjson_val* object, lsm_tree_schema_t& schema) noexcept {
    return read_json(yyjson_obj_get(object, "lsm_levels"), schema.lsm_levels);
}

[[nodiscard]] inline bool read_json(yyjson_val* object, collection_schema_t& schema) noexcept {
    return read_json(yyjson_obj_get(object, "lsm_tree"), schema.lsm_tree);
}

[[nodiscard]] inline bool read_json(yyjson_val* object, collection_snapshot_schema_t& schema) noexcept {
    return read_json(yyjson_obj_get(object, "lsm_tree"), schema.lsm_tree) &&
           read_json(yyjson_obj_get(object, "source_id"), schema.source_id);
}

[[nodiscard]] inline bool read_json(yyjson_val* object,
                                    single_collection_database_schema_t::snapshot_t& snapshot) noexcept {
    return read_json(yyjson_obj_get(object, "collection"), snapshot.collection) &&
           read_json(yyjson_obj_get(object, "id"), snapshot.id) &&
           read_json(yyjson_obj_get(object, "time_stamp"), snapshot.time_stamp);
}

[[nodiscard]] inline bool read_json(yyjson_val* object, single_collection_database_schema_t& schema) noexcept {
    return read_json(yyjson_obj_get(object, "id"), schema.id) &&
           read_json(yyjson_obj_get(object, "collection"), schema.collection) &&
           read_json(yyjson_obj_get(object, "snapshots"), schema.snapshots);
}

[[nodiscard]] inline bool read_json(yyjson_val* object, database_schema_t::collection_t& collection) noexcept {
    return read_json(yyjson_obj_get(object, "config"), collection.config) &&
           read_json(yyjson_obj_get(object, "schema"), collection.schema) &&
           read_json(yyjson_obj_get(object, "name"), collection.name) &&
           read_json(yyjson_obj_get(object, "id"), collection.id) &&
           read_json(yyjson_obj_get(object, "type"), collection.type);
}

[[nodiscard]] inline bool read_json(yyjson_val* object,
                                    database_schema_t::snapshot_t::collection_t& collection) noexcept {
    return read_json(yyjson_obj_get(object, "source_config"), collection.source_config) &&
           read_json(yyjson_obj_get(object, "source_name"), collection.source_name) &&
           read_json(yyjson_obj_get(object, "source_type"), collection.source_type) &&
           read_json(yyjson_obj_get(object, "id"), collection.id) &&
           read_json(yyjson_obj_get(object, "collection"), collection.collection);
}

[[nodiscard]] inline bool read_json(yyjson_val* object, database_schema_t::snapshot_t& snapshot) noexcept {
    return read_json(yyjson_obj_get(object, "id"), snapshot.id) &&
           read_json(yyjson_obj_get(object, "time_stamp"), snapshot.time_stamp) &&
           read_json(yyjson_obj_get(object, "collections"), snapshot.collections);
}

[[nodiscard]] inline bool read_json(yyjson_val* object, database_schema_t& schema) noexcept {
    return read_json(yyjson_obj_get(object, "id"), schema.id) &&
           read_json(yyjson_obj_get(object, "collections"), schema.collections) &&
           read_json(yyjson_obj_get(object, "snapshots"), schema.snapshots);
}

/** Reads the version and kinds at the root of a schema document. */
inline status_t read_schema_header(yyjson_val* root, schema_header_t& header) noexcept {
    if (status_t const status = read_json_version(root); status.failed()) return status;
    std::uint8_t schema_kind = 0;
    std::uint8_t database_kind = 0;
    if (!read_json(yyjson_obj_get(root, "schema_kind"), schema_kind) ||
        !read_json(yyjson_obj_get(root, "database_kind"), database_kind))
        return status_t {status_code_t::malformed_k, "Invalid schema"};
    header.schema_kind = static_cast<schema_kind_t>(schema_kind);
    header.database_kind = static_cast<database_kind_t>(database_kind);
    return status_t {};
}

/** Reads the version and kinds of the schema document at @p file_path. */
inline status_t load_schema_header(char const* file_path, schema_header_t& header) noexcept {
    yyjson_read_err error;
    yyjson_doc* const document = yyjson_read_file(file_path, YYJSON_READ_NOFLAG, nullptr, &error);
    if (!document && error.code == YYJSON_READ_ERROR_FILE_OPEN)
        return status_t {status_code_t::not_found_k, "File not exists"};
    if (!document) return status_t {status_code_t::malformed_k, "Invalid schema"};
    status_t const status = read_schema_header(yyjson_doc_get_root(document), header);
    yyjson_doc_free(document);
    return status;
}

/** Reads the schema document at @p file_path, rejecting one of another kind. */
template <database_schema_concept schema_>
status_t load_schema(char const* file_path, schema_& schema) noexcept {
    yyjson_read_err error;
    yyjson_doc* const document = yyjson_read_file(file_path, YYJSON_READ_NOFLAG, nullptr, &error);
    if (!document && error.code == YYJSON_READ_ERROR_FILE_OPEN)
        return status_t {status_code_t::not_found_k, "File not exists"};
    if (!document) return status_t {status_code_t::malformed_k, "Invalid schema"};

    yyjson_val* const root = yyjson_doc_get_root(document);
    schema_header_t header;
    status_t status = read_schema_header(root, header);
    if (status && (header.schema_kind != schema_::schema_kind_k || header.database_kind != schema_::database_kind_k))
        status = status_t {status_code_t::malformed_k, "Invalid schema"};
    if (status && !read_json(yyjson_obj_get(root, "schema"), schema))
        status = status_t {status_code_t::malformed_k, "Invalid schema"};
    yyjson_doc_free(document);
    return status;
}

#pragma endregion Reading

#pragma region Writing

/** Adds @p value to @p object under the literal @p key, answering whether yyjson allocated every part. */
[[nodiscard]] inline bool add_json_member(yyjson_mut_doc* document, yyjson_mut_val* object, char const* key,
                                          yyjson_mut_val* value) noexcept {
    return object && value && yyjson_mut_obj_add(object, yyjson_mut_str(document, key), value);
}

template <std::unsigned_integral value_>
[[nodiscard]] yyjson_mut_val* write_json(yyjson_mut_doc* document, value_ number) noexcept {
    return yyjson_mut_uint(document, number);
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document, string_t const& text) noexcept {
    return yyjson_mut_str(document, text.c_str());
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document, uuid_t const& id) noexcept {
    char text[uuid_t::printed_length_k] = {};
    id.print(text);
    return yyjson_mut_strncpy(document, text, uuid_t::printed_length_k);
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document, collection_id_t const& id) noexcept {
    char text[uuid_text_length_k];
    uuid_to_text(id.uuid, text);
    return yyjson_mut_strncpy(document, text, uuid_text_length_k);
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document, time_point_t time) noexcept {
    return yyjson_mut_sint(document, static_cast<std::int64_t>(time.time_since_epoch().count()));
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document, value_layout_t value_layout) noexcept {
    return yyjson_mut_uint(document, static_cast<std::uint16_t>(value_layout));
}

template <typename element_, template <typename> typename allocator_>
[[nodiscard]] yyjson_mut_val* write_json(yyjson_mut_doc* document,
                                         st::basic_vector<element_, allocator_<element_>> const& elements) noexcept {
    yyjson_mut_val* const array = yyjson_mut_arr(document);
    if (!array) return nullptr;
    for (element_ const& element : elements)
        if (!yyjson_mut_arr_append(array, write_json(document, element))) return nullptr;
    return array;
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document, sorted_run_config_t const& config) noexcept {
    yyjson_mut_val* const object = yyjson_mut_obj(document);
    bool const written = add_json_member(document, object, "value_size", write_json(document, config.value_size)) &&
                         add_json_member(document, object, "value_max_size",
                                         write_json(document, config.value_max_size));
    return written ? object : nullptr;
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document, lsm_level_config_t const& config) noexcept {
    yyjson_mut_val* const object = yyjson_mut_obj(document);
    return add_json_member(document, object, "sorted_run", write_json(document, config.sorted_run)) ? object : nullptr;
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document, lsm_tree_config_t const& config) noexcept {
    yyjson_mut_val* const object = yyjson_mut_obj(document);
    bool const written = add_json_member(document, object, "lsm_level_0", write_json(document, config.lsm_level_0)) &&
                         add_json_member(document, object, "level_enlarge_factor",
                                         write_json(document, config.level_enlarge_factor));
    return written ? object : nullptr;
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document, collection_config_t const& config) noexcept {
    yyjson_mut_val* const object = yyjson_mut_obj(document);
    bool const written = add_json_member(document, object, "lsm_tree", write_json(document, config.lsm_tree)) &&
                         add_json_member(document, object, "concurrency_limit",
                                         write_json(document, config.concurrency_limit)) &&
                         add_json_member(document, object, "cache_limit", write_json(document, config.cache_limit)) &&
                         add_json_member(document, object, "memory_runs_max_bytes",
                                         write_json(document, config.memory_runs_max_bytes)) &&
                         add_json_member(document, object, "memory_runs_max_deltas",
                                         write_json(document, config.memory_runs_max_deltas));
    return written ? object : nullptr;
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document, sorted_run_schema_t const& schema) noexcept {
    yyjson_mut_val* const object = yyjson_mut_obj(document);
    return add_json_member(document, object, "file_name", write_json(document, schema.file_name)) ? object : nullptr;
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document, lsm_level_schema_t const& schema) noexcept {
    yyjson_mut_val* const object = yyjson_mut_obj(document);
    return add_json_member(document, object, "sorted_runs", write_json(document, schema.sorted_runs)) ? object
                                                                                                      : nullptr;
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document, lsm_tree_schema_t const& schema) noexcept {
    yyjson_mut_val* const object = yyjson_mut_obj(document);
    return add_json_member(document, object, "lsm_levels", write_json(document, schema.lsm_levels)) ? object : nullptr;
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document, collection_schema_t const& schema) noexcept {
    yyjson_mut_val* const object = yyjson_mut_obj(document);
    return add_json_member(document, object, "lsm_tree", write_json(document, schema.lsm_tree)) ? object : nullptr;
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document,
                                                collection_snapshot_schema_t const& schema) noexcept {
    yyjson_mut_val* const object = yyjson_mut_obj(document);
    bool const written = add_json_member(document, object, "lsm_tree", write_json(document, schema.lsm_tree)) &&
                         add_json_member(document, object, "source_id", write_json(document, schema.source_id));
    return written ? object : nullptr;
}

[[nodiscard]] inline yyjson_mut_val* write_json(
    yyjson_mut_doc* document, single_collection_database_schema_t::snapshot_t const& snapshot) noexcept {
    yyjson_mut_val* const object = yyjson_mut_obj(document);
    bool const written = add_json_member(document, object, "collection", write_json(document, snapshot.collection)) &&
                         add_json_member(document, object, "id", write_json(document, snapshot.id)) &&
                         add_json_member(document, object, "time_stamp", write_json(document, snapshot.time_stamp));
    return written ? object : nullptr;
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document,
                                                single_collection_database_schema_t const& schema) noexcept {
    yyjson_mut_val* const object = yyjson_mut_obj(document);
    bool const written = add_json_member(document, object, "id", write_json(document, schema.id)) &&
                         add_json_member(document, object, "collection", write_json(document, schema.collection)) &&
                         add_json_member(document, object, "snapshots", write_json(document, schema.snapshots));
    return written ? object : nullptr;
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document,
                                                database_schema_t::collection_t const& collection) noexcept {
    yyjson_mut_val* const object = yyjson_mut_obj(document);
    bool const written = add_json_member(document, object, "config", write_json(document, collection.config)) &&
                         add_json_member(document, object, "schema", write_json(document, collection.schema)) &&
                         add_json_member(document, object, "name", write_json(document, collection.name)) &&
                         add_json_member(document, object, "id", write_json(document, collection.id)) &&
                         add_json_member(document, object, "type", write_json(document, collection.type));
    return written ? object : nullptr;
}

[[nodiscard]] inline yyjson_mut_val* write_json(
    yyjson_mut_doc* document, database_schema_t::snapshot_t::collection_t const& collection) noexcept {
    yyjson_mut_val* const object = yyjson_mut_obj(document);
    bool const written =
        add_json_member(document, object, "source_config", write_json(document, collection.source_config)) &&
        add_json_member(document, object, "source_name", write_json(document, collection.source_name)) &&
        add_json_member(document, object, "source_type", write_json(document, collection.source_type)) &&
        add_json_member(document, object, "id", write_json(document, collection.id)) &&
        add_json_member(document, object, "collection", write_json(document, collection.collection));
    return written ? object : nullptr;
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document,
                                                database_schema_t::snapshot_t const& snapshot) noexcept {
    yyjson_mut_val* const object = yyjson_mut_obj(document);
    bool const written = add_json_member(document, object, "id", write_json(document, snapshot.id)) &&
                         add_json_member(document, object, "time_stamp", write_json(document, snapshot.time_stamp)) &&
                         add_json_member(document, object, "collections", write_json(document, snapshot.collections));
    return written ? object : nullptr;
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document, database_schema_t const& schema) noexcept {
    yyjson_mut_val* const object = yyjson_mut_obj(document);
    bool const written = add_json_member(document, object, "id", write_json(document, schema.id)) &&
                         add_json_member(document, object, "collections", write_json(document, schema.collections)) &&
                         add_json_member(document, object, "snapshots", write_json(document, schema.snapshots));
    return written ? object : nullptr;
}

/** Writes @p schema as an indented document at @p file_path, replacing any file there. */
template <database_schema_concept schema_>
status_t save_schema(char const* file_path, schema_ const& schema) noexcept {
    yyjson_mut_doc* const document = yyjson_mut_doc_new(nullptr);
    if (!document) return status_t {status_code_t::out_of_memory_k, "Failed to allocate memory"};

    yyjson_mut_val* const root = yyjson_mut_obj(document);
    bool const written =
        add_json_member(document, root, "version", yyjson_mut_str(document, format_version_k)) &&
        add_json_member(document, root, "schema_kind",
                        yyjson_mut_uint(document, static_cast<std::uint8_t>(schema_::schema_kind_k))) &&
        add_json_member(document, root, "database_kind",
                        yyjson_mut_uint(document, static_cast<std::uint8_t>(schema_::database_kind_k))) &&
        add_json_member(document, root, "schema", write_json(document, schema));
    if (written) yyjson_mut_doc_set_root(document, root);
    bool const saved = written && yyjson_mut_write_file(file_path, document, YYJSON_WRITE_PRETTY, nullptr, nullptr);
    yyjson_mut_doc_free(document);

    if (!written) return status_t {status_code_t::out_of_memory_k, "Failed to allocate memory"};
    if (!saved) return status_t {status_code_t::io_k, "Failed to submit file request"};
    return status_t {};
}

#pragma endregion Writing

#pragma region Validation

/** How much an issue found during validation matters. */
enum class issue_kind_t : std::uint8_t {
    warning_k,
    error_k,
};

/** A callable validation hands every issue to, with a message that lives only during the call. */
template <typename sink_>
concept issue_sink_concept = std::invocable<sink_&, issue_kind_t, std::string_view>;

/** Formats an issue on the stack and hands it to @p sink, answering whether it was only a warning. */
template <issue_sink_concept sink_, typename... arguments_>
bool report_issue(sink_& sink, issue_kind_t kind, fmt::format_string<arguments_...> format,
                  arguments_&&... arguments) noexcept {
    char message[256];
    auto const formatted = fmt::format_to_n(message, sizeof(message), format, std::forward<arguments_>(arguments)...);
    sink(kind, std::string_view(message, formatted.out - message));
    return kind == issue_kind_t::warning_k;
}

/** Checks resources on their own, answering whether no error was reported. */
template <issue_sink_concept sink_>
[[nodiscard]] bool validate(resources_config_t const& config, sink_&& sink) noexcept {
    if (config.memory_limit == 0) return report_issue(sink, issue_kind_t::error_k, "Memory limit is not set");
    if (config.gpu_devices.empty() && config.gpu_memory_limit > 0)
        return report_issue(sink, issue_kind_t::error_k, "GPU memory limit is set but no GPU device is selected");
    if (!config.gpu_devices.empty() && config.gpu_memory_limit < 4 * 1024 * 1024)
        return report_issue(sink, issue_kind_t::error_k, "GPU memory limit leaves no room for device buffers");
    if (config.directory.empty()) return report_issue(sink, issue_kind_t::error_k, "Metadata directory is not set");
    if (config.channels.depth == 0) return report_issue(sink, issue_kind_t::error_k, "I/O depth is not set");
    if (config.channels.request_buffers_count == 0 || config.channels.stream_buffers_count == 0)
        return report_issue(sink, issue_kind_t::error_k, "A channel needs a request block and a streaming block");
    if (config.channels.request_buffer_bytes == 0 || config.channels.stream_buffer_bytes == 0)
        return report_issue(sink, issue_kind_t::error_k, "I/O blocks leave a channel nothing to transfer through");
    if (config.shared_io_channels == 0)
        return report_issue(sink, issue_kind_t::error_k,
                            "A mount keeps a channel for the threads outside its executor");

    for (std::size_t index = 0; index < config.data_directories.size(); ++index)
        for (std::size_t later = index + 1; later < config.data_directories.size(); ++later)
            if (config.data_directories[index].path == config.data_directories[later].path)
                return report_issue(sink, issue_kind_t::error_k, "Data directory is listed twice: {}",
                                    config.data_directories[index].path.c_str());
    return true;
}

/** Checks resources, then an explicit thread count against @c executor_t::logical_cores_count. */
template <issue_sink_concept sink_>
[[nodiscard]] bool validate(resources_config_t const& config, std::size_t logical_cores_count, sink_&& sink) noexcept {
    if (!validate(config, sink)) return false;
    if (config.threads > logical_cores_count)
        return report_issue(sink, issue_kind_t::error_k, "{} threads exceed the {} logical cores", config.threads,
                            logical_cores_count);
    if (config.threads == logical_cores_count)
        report_issue(sink, issue_kind_t::warning_k, "Threads take every logical core");
    return true;
}

/** Checks the delta limits of one transaction against the storage limits. */
template <issue_sink_concept sink_>
[[nodiscard]] bool validate(transaction_config_t const& config, sink_&& sink) noexcept {
    if (config.max_deltas < storage_limits_t::transaction_min_deltas_k ||
        config.max_deltas > storage_limits_t::transaction_max_deltas_k)
        return report_issue(sink, issue_kind_t::error_k, "Transaction deltas must be within [{}, {}]",
                            storage_limits_t::transaction_min_deltas_k, storage_limits_t::transaction_max_deltas_k);
    if (config.max_bytes < storage_limits_t::transaction_min_bytes_k ||
        config.max_bytes > storage_limits_t::transaction_max_bytes_k)
        return report_issue(sink, issue_kind_t::error_k, "Transaction bytes must be within [{}, {}]",
                            storage_limits_t::transaction_min_bytes_k, storage_limits_t::transaction_max_bytes_k);
    return true;
}

/** Checks the limits of one collection holding values of @p value_layout against the storage limits. */
template <issue_sink_concept sink_>
[[nodiscard]] bool validate(user_collection_config_t const& config, value_layout_t value_layout,
                            sink_&& sink) noexcept {
    if ((config.value_size == 0) == (config.value_max_size == 0))
        return report_issue(sink, issue_kind_t::error_k, "Exactly one of value_size and value_max_size must be set");

    if (is_inline(value_layout)) {
        auto const smallest = static_cast<stored_value_size_t>(value_layout_t::inline_1_byte_k);
        auto const largest = static_cast<stored_value_size_t>(value_layout_t::inline_64_bytes_k);
        if (config.value_size < smallest || config.value_size > largest)
            return report_issue(sink, issue_kind_t::error_k, "Inline value size must be within [{}, {}]", smallest,
                                largest);
        if (config.value_size > static_cast<stored_value_size_t>(value_layout))
            return report_issue(sink, issue_kind_t::error_k, "Value layout is too small for the value size");
        if (config.value_size <= static_cast<stored_value_size_t>(value_layout) / 2)
            report_issue(sink, issue_kind_t::warning_k, "A smaller value layout fits the value size");
        if (config.cache_limit > 0)
            report_issue(sink, issue_kind_t::warning_k, "Value cache does not serve inline values");
    }
    else if (value_layout == value_layout_t::fixed_width_k) {
        if (config.value_size < storage_limits_t::fixed_value_min_size_k ||
            config.value_size > storage_limits_t::fixed_value_max_size_k)
            return report_issue(sink, issue_kind_t::error_k, "Fixed value size must be within [{}, {}]",
                                storage_limits_t::fixed_value_min_size_k, storage_limits_t::fixed_value_max_size_k);
        if (config.value_size <= static_cast<stored_value_size_t>(value_layout_t::inline_64_bytes_k))
            report_issue(sink, issue_kind_t::warning_k, "An inline value layout fits the value size");
    }
    else if (config.value_max_size < storage_limits_t::variable_value_min_size_k ||
             config.value_max_size > storage_limits_t::variable_value_max_size_k)
        return report_issue(sink, issue_kind_t::error_k, "Variable value size must be within [{}, {}]",
                            storage_limits_t::variable_value_min_size_k, storage_limits_t::variable_value_max_size_k);

    std::size_t const value_max_size = std::max(config.value_size, config.value_max_size);
    if (!is_inline(value_layout)) {
        if (config.cache_limit > storage_limits_t::cache_limit_max_size_k)
            return report_issue(sink, issue_kind_t::error_k, "Value cache must not exceed {} bytes",
                                storage_limits_t::cache_limit_max_size_k);
        if (config.cache_limit / value_max_size > std::numeric_limits<std::uint32_t>::max())
            report_issue(sink, issue_kind_t::warning_k, "Value cache holds more values than it can index");
    }

    if (config.concurrency_limit == 0 || config.concurrency_limit >= config.memory_runs_max_deltas)
        return report_issue(sink, issue_kind_t::error_k,
                            "Concurrency limit must be within [1, memory_runs_max_deltas)");
    if (config.memory_runs_max_deltas > storage_limits_t::memory_runs_max_deltas_k)
        return report_issue(sink, issue_kind_t::error_k, "Memory run deltas must not exceed {}",
                            storage_limits_t::memory_runs_max_deltas_k);
    if (config.memory_runs_max_bytes < storage_limits_t::memory_runs_min_bytes_k ||
        config.memory_runs_max_bytes > storage_limits_t::memory_runs_max_bytes_k)
        return report_issue(sink, issue_kind_t::error_k, "Memory run bytes must be within [{}, {}]",
                            storage_limits_t::memory_runs_min_bytes_k, storage_limits_t::memory_runs_max_bytes_k);
    if (config.memory_runs_max_bytes < value_max_size)
        return report_issue(sink, issue_kind_t::error_k, "Memory run bytes cannot hold the largest value");
    if (config.level_enlarge_factor < storage_limits_t::level_min_enlarge_factor_k ||
        config.level_enlarge_factor > storage_limits_t::level_max_enlarge_factor_k)
        return report_issue(sink, issue_kind_t::error_k, "Level enlarge factor must be within [{}, {}]",
                            storage_limits_t::level_min_enlarge_factor_k, storage_limits_t::level_max_enlarge_factor_k);
    return true;
}

/** Checks resources and the limits of a database within them. */
template <issue_sink_concept sink_>
[[nodiscard]] bool validate(resources_config_t const& resources, database_config_t const& config,
                            sink_&& sink) noexcept {
    if (!validate(resources, sink) || !validate(config.default_transaction, sink)) return false;
    if (resources.memory_limit < config.default_transaction.max_bytes)
        return report_issue(sink, issue_kind_t::error_k, "Memory limit cannot hold one transaction");
    return true;
}

/** Checks resources and the limits of a single-collection database within them. */
template <issue_sink_concept sink_>
[[nodiscard]] bool validate(resources_config_t const& resources, user_single_collection_database_config_t const& config,
                            sink_&& sink) noexcept {
    if (!validate(resources, sink) || !validate(config.default_transaction, sink)) return false;
    if (config.default_transaction.max_deltas > config.collection.memory_runs_max_deltas ||
        config.default_transaction.max_bytes > config.collection.memory_runs_max_bytes)
        return report_issue(sink, issue_kind_t::error_k,
                            "Transaction limits exceed the single-collection database's memory run bounds");
    if (!validate(config.collection, resolve_value_layout(config.collection.value_size), sink)) return false;
    if (resources.memory_limit < config.default_transaction.max_bytes + config.collection.memory_runs_max_bytes)
        return report_issue(sink, issue_kind_t::error_k, "Memory limit cannot hold a transaction and the memory runs");
    return true;
}

/** Checks resources and the limits of a collection within them. */
template <issue_sink_concept sink_>
[[nodiscard]] bool validate(resources_config_t const& resources, user_collection_config_t const& config,
                            sink_&& sink) noexcept {
    if (!validate(resources, sink) || !validate(config, resolve_value_layout(config.value_size), sink)) return false;
    if (resources.memory_limit < config.memory_runs_max_bytes)
        return report_issue(sink, issue_kind_t::error_k, "Memory limit cannot hold the memory runs");
    return true;
}

#pragma endregion Validation

#pragma endregion Configurations and Schemas

#pragma region LSM Level

#pragma region Vocabulary

/** Whether a level's runs may overlap, which decides how a lookup and a scan order them. */
enum class lsm_level_order_t : std::uint8_t {

    /** Runs written by flushes, which overlap, so the youngest run holding a key carries its value. */
    overlapping_k,

    /** Runs written by a merge, which are disjoint and ascending, so at most one run holds a key. */
    sorted_k,
};

/** Whether anything is stored below a level, which decides whether a merge into it may drop remove deltas. */
enum class lsm_level_depth_t : std::uint8_t {

    /** Levels sit below this one, so a remove delta still shadows what they hold. */
    intermediate_k,

    /** Nothing sits below this one, so a remove delta shadows nothing and is dropped for good. */
    bottom_k,
};

/** Where one key sits inside a level, which is the run that holds it and its place in that run. */
struct lsm_level_position_t {

    /** The run of the level that holds the key. */
    std::uint32_t run_index = 0;

    /** The sorted position of the key inside that run. */
    std::uint64_t key_slot_position = 0;
};

#pragma endregion Vocabulary

#pragma region Level

/**
 *  @brief The sorted runs of one level, answering lookups and scans over all of them at once.
 *
 *  Every run is one heap allocation the level owns, so a successor level set copies the pointer rather
 *  than the index behind it and a reader of either set reads the same bytes.
 *
 *  @tparam waiter_type_ What the core does while a run waits for its reads.
 */
template <typename key_slot_type_, typename volume_type_, typename waiter_type_>
class lsm_level {
  public:

#pragma region Types

    /** The key slot the runs of this level hold. */
    using key_slot_t = key_slot_type_;

    /** The volume the runs of this level live on. */
    using volume_t = volume_type_;

    /** One immutable run of this level. */
    using sorted_run_t = lsm_sorted_run<key_slot_t, volume_t, waiter_type_>;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** How the values of this level are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

    /** Whether the values sit in the key slots themselves, so reading one needs no I/O. */
    static constexpr bool holds_values_inline_k = is_inline(value_layout_k);

#pragma endregion Types

#pragma region Construction

    lsm_level() noexcept = default;
    lsm_level(lsm_level const&) = delete;
    lsm_level& operator=(lsm_level const&) = delete;

    lsm_level(lsm_level&& other) noexcept
        : runs_(std::move(other.runs_)), number_(other.number_), order_(other.order_),
          size_bytes_(std::exchange(other.size_bytes_, 0)),
          population_count_(std::exchange(other.population_count_, 0)),
          lower_(std::exchange(other.lower_, limit<stored_key_t>::max())),
          upper_(std::exchange(other.upper_, limit<stored_key_t>::min())) {}

    lsm_level& operator=(lsm_level&& other) noexcept {
        if (this == &other) return *this;
        discard();
        runs_ = std::move(other.runs_);
        number_ = other.number_;
        order_ = other.order_;
        size_bytes_ = std::exchange(other.size_bytes_, 0);
        population_count_ = std::exchange(other.population_count_, 0);
        lower_ = std::exchange(other.lower_, limit<stored_key_t>::max());
        upper_ = std::exchange(other.upper_, limit<stored_key_t>::min());
        return *this;
    }

    ~lsm_level() noexcept { discard(); }

    /** An empty level numbered @p number whose runs are read in the order @p order names. */
    [[nodiscard]] static lsm_level of(std::uint32_t number, lsm_level_order_t order) noexcept {
        lsm_level level;
        level.number_ = number;
        level.order_ = order;
        return level;
    }

    /** Takes @p run into the level, at the back for an overlapping level and in key order for a sorted one. */
    status_t bind_queue_pair(sorted_run_t&& run) noexcept {
        if (runs_.reserve(runs_.size() + 1) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A level could not hold another run"};
        std::size_t position = runs_.size();
        if (order_ == lsm_level_order_t::sorted_k) {
            position = 0;
            while (position != runs_.size() && runs_[position]->upper_stored_key() < run.lower_stored_key()) ++position;
            if (position != runs_.size() && !(run.upper_stored_key() < runs_[position]->lower_stored_key()))
                return {status_code_t::invalid_argument_k, "A sorted level takes runs that overlap no other"};
        }
        sorted_run_t* held = new (std::nothrow) sorted_run_t {std::move(run)};
        if (held == nullptr) return {status_code_t::out_of_memory_k, "A level could not hold another run"};
        note_(*held);
        [[maybe_unused]] st::status_t const placed = runs_.insert(position, std::move(held));
        return {};
    }

    /** Names every run @p other names, which is how a prepared level set carries an untouched level. */
    status_t carry(lsm_level const& other) noexcept {
        if (runs_.reserve(runs_.size() + other.runs_.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A level could not name that many runs"};
        for (std::size_t index = 0; index != other.runs_.size(); ++index) {
            sorted_run_t* named = other.runs_[index];
            note_(*named);
            [[maybe_unused]] st::status_t const placed = runs_.push_back(std::move(named));
        }
        return {};
    }

    /** Makes room for @p count more runs, so a merge that plans them all fails before it writes any. */
    status_t reserve(std::size_t count) noexcept {
        if (runs_.reserve(runs_.size() + count) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A level could not hold that many runs"};
        return {};
    }

    /** Forgets every run without freeing it, which only a level set handing its runs on may do. */
    void relinquish() noexcept {
        runs_.clear();
        forget_();
    }

    /** Frees every run's memory without touching the medium, which the destructor does anyway. */
    void discard() noexcept {
        for (std::size_t index = 0; index != runs_.size(); ++index) delete runs_[index];
        runs_.clear();
        forget_();
    }

    /** Returns every run's bytes to @p volume and then frees them, which destroys the level's data. */
    status_t drop(volume_t& volume, io_channel_t& channel) noexcept {
        status_t first;
        for (std::size_t index = 0; index != runs_.size(); ++index)
            if (status_t const dropped = sorted_run_t::drop(volume, channel, runs_[index]->content_id());
                dropped.failed() && first.succeeded())
                first = dropped;
        discard();
        return first;
    }

#pragma endregion Construction

#pragma region Shape

    /** Where this level sits in its tree, zero being the level flushes land on. */
    [[nodiscard]] std::uint32_t number() const noexcept { return number_; }

    /** Whether this level's runs may overlap. */
    [[nodiscard]] lsm_level_order_t order() const noexcept { return order_; }

    /** Runs the level holds. */
    [[nodiscard]] std::size_t size() const noexcept { return runs_.size(); }

    /** Whether the level holds no run at all. */
    [[nodiscard]] bool empty() const noexcept { return runs_.empty(); }

    /** Key slots every run of the level holds together, one key counted once per run that holds it. */
    [[nodiscard]] std::uint64_t population_count() const noexcept { return population_count_; }

    /** Bytes every run of the level takes on its volume. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept { return size_bytes_; }

    /** The least key any run of the level holds, or the greatest key of the type for an empty level. */
    [[nodiscard]] stored_key_t lower_stored_key() const noexcept { return lower_; }

    /** The greatest key any run of the level holds, or the least key of the type for an empty level. */
    [[nodiscard]] stored_key_t upper_stored_key() const noexcept { return upper_; }

    /** The run at @p index, which for an overlapping level is ordered oldest first. */
    [[nodiscard]] sorted_run_t const& run(std::size_t index) const noexcept { return *runs_[index]; }

#pragma endregion Shape

#pragma region Lookups

    /**
     *  @brief Whether the level carries @p stored_key, and whether the key slot holding it locates a value.
     *  @param[in] stored_key The key to look for.
     *  @param[out] position Where the key sits, which is meaningless when the answer is @c missing_k.
     */
    [[nodiscard]] lookup_result_t find(stored_key_t stored_key, lsm_level_position_t& position) const noexcept {
        return find(stored_key, limit<std::uint64_t>::max(), position);
    }

    /** As @c find, answering the newest version stamped at or below @p stamp, which a reader pinned there sees. */
    [[nodiscard]] lookup_result_t find(stored_key_t stored_key, std::uint64_t stamp,
                                       lsm_level_position_t& position) const noexcept {
        position = lsm_level_position_t {};
        if (order_ == lsm_level_order_t::overlapping_k) {
            for (std::size_t index = runs_.size(); index-- != 0;) {
                sorted_run_t const& run = *runs_[index];
                if (run.population_count() == 0) continue;
                if (stored_key < run.lower_stored_key() || run.upper_stored_key() < stored_key) continue;
                std::uint64_t at = 0;
                lookup_result_t const found = run.find(stored_key, stamp, at);
                if (found == lookup_result_t::missing_k) continue;
                position = lsm_level_position_t {static_cast<std::uint32_t>(index), at};
                return found;
            }
            return lookup_result_t::missing_k;
        }

        std::size_t low = 0;
        std::size_t high = runs_.size();
        while (low != high) {
            std::size_t const middle = low + (high - low) / 2;
            sorted_run_t const& run = *runs_[middle];
            if (run.upper_stored_key() < stored_key) {
                low = middle + 1;
                continue;
            }
            if (stored_key < run.lower_stored_key()) {
                high = middle;
                continue;
            }
            std::uint64_t at = 0;
            lookup_result_t const found = run.find(stored_key, stamp, at);
            if (found != lookup_result_t::missing_k)
                position = lsm_level_position_t {static_cast<std::uint32_t>(middle), at};
            return found;
        }
        return lookup_result_t::missing_k;
    }

    /** The key slot at @p position, which must have come from a @c find that answered anything but missing. */
    [[nodiscard]] key_slot_t const& key_slot(lsm_level_position_t position) const noexcept {
        return runs_[position.run_index]->key_slot(position.key_slot_position);
    }

    /** Where the value of the key slot at @p position sits, empty for one that needs no read. */
    [[nodiscard]] value_window_t value_window(lsm_level_position_t position) const noexcept {
        return runs_[position.run_index]->value_window(position.key_slot_position);
    }

    /**
     *  @brief Calls @p visitor with the position and the key slot of every key, in ascending order of keys.
     *
     *  @note An overlapping level reports one position per key, the youngest run's, and skips the versions
     *      the older runs still carry; a sorted level walks its runs one after another.
     */
    template <typename visitor_type_>
    status_t for_each_key_slot(visitor_type_&& visitor) const noexcept {
        if (order_ == lsm_level_order_t::sorted_k) {
            for (std::size_t index = 0; index != runs_.size(); ++index)
                runs_[index]->for_each_key_slot([&](std::uint64_t at, key_slot_t const& slot) noexcept {
                    visitor(lsm_level_position_t {static_cast<std::uint32_t>(index), at}, slot);
                });
            return {};
        }

        st::basic_vector<std::uint64_t> cursors;
        if (cursors.resize(runs_.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A level could not track a cursor per run"};
        for (std::size_t index = 0; index != runs_.size(); ++index) cursors[index] = 0;

        for (;;) {
            std::size_t chosen = runs_.size();
            stored_key_t smallest = limit<stored_key_t>::max();
            for (std::size_t index = 0; index != runs_.size(); ++index) {
                if (cursors[index] == runs_[index]->population_count()) continue;
                stored_key_t const key = runs_[index]->key_slot(cursors[index]).stored_key;
                if (chosen != runs_.size() && smallest < key) continue;
                chosen = index;
                smallest = key;
            }
            if (chosen == runs_.size()) return {};

            visitor(lsm_level_position_t {static_cast<std::uint32_t>(chosen), cursors[chosen]},
                    runs_[chosen]->key_slot(cursors[chosen]));
            for (std::size_t index = 0; index != runs_.size(); ++index)
                if (cursors[index] != runs_[index]->population_count() &&
                    runs_[index]->key_slot(cursors[index]).stored_key == smallest)
                    ++cursors[index];
        }
    }

#pragma endregion Lookups

  private:

#pragma region Private Methods

    /** Folds @p run into the bounds, the byte count and the population the level reports. */
    void note_(sorted_run_t const& run) noexcept {
        size_bytes_ += run.size_bytes();
        population_count_ += run.population_count();
        if (run.lower_stored_key() < lower_) lower_ = run.lower_stored_key();
        if (upper_ < run.upper_stored_key()) upper_ = run.upper_stored_key();
    }

    /** Resets everything the runs contributed, which both ways of emptying a level need. */
    void forget_() noexcept {
        size_bytes_ = 0;
        population_count_ = 0;
        lower_ = limit<stored_key_t>::max();
        upper_ = limit<stored_key_t>::min();
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The runs of the level, oldest first when they overlap and ascending when they do not. */
    st::basic_vector<sorted_run_t*> runs_;

    /** Where this level sits in its tree. */
    std::uint32_t number_ = 0;

    /** Whether the runs may overlap. */
    lsm_level_order_t order_ = lsm_level_order_t::sorted_k;

    /** Bytes every run takes, kept as runs are adopted rather than recounted. */
    std::uint64_t size_bytes_ = 0;

    /** Key slots every run holds, kept as runs are adopted rather than recounted. */
    std::uint64_t population_count_ = 0;

    /** The least key any run holds. */
    stored_key_t lower_ = limit<stored_key_t>::max();

    /** The greatest key any run holds. */
    stored_key_t upper_ = limit<stored_key_t>::min();

#pragma endregion Private Members
};

#pragma endregion Level

#pragma endregion LSM Level

#pragma region LSM Level Merger

#pragma region Vocabulary

/** Where a merge runs its compaction, which a caller either fixes or leaves to the merge. */
enum class compaction_placement_t : std::uint8_t {

    /** A device when one is present and the merge is large enough, and the host otherwise. */
    automatic_k,

    /** The host, whatever devices the machine carries. */
    host_k,

    /** A device, which a build without CUDA refuses rather than falling back. */
    device_k,
};

/** Which compactor a merge takes, once the placement and the size of the merge are both known. */
enum class compactor_choice_t : std::uint8_t {

    /** @c host_compactor_t, on the calling thread. */
    host_k,

    /** @c cuda_compactor_t, on the first device the executor named. */
    device_k,
};

/**
 *  @brief The word a caller raises to stop a merge, which the merge reads between blocks.
 *
 *  A cancelled merge leaves the level set it was asked to replace untouched and returns every block and
 *  every reservation it took, so cancelling one costs the work it had done and nothing else.
 *
 *  @tparam atomic_reference_ The reference the word is owned through.
 */
template <template <typename> typename atomic_reference_>
class compaction_cancellation {
  public:

    compaction_cancellation() noexcept = default;
    compaction_cancellation(compaction_cancellation const&) = delete;
    compaction_cancellation& operator=(compaction_cancellation const&) = delete;

    /** Raises the word, which the merge reads at its next block boundary. */
    void raise() noexcept {
        atomic_reference_<std::int32_t> {raised_}.store(static_cast<std::int32_t>(cancellation_state_t::cancelled_k),
                                                        std::memory_order_release);
    }

    /** Whether a caller has raised the word. */
    [[nodiscard]] bool cancelled() const noexcept {
        return atomic_reference_<std::int32_t const> {raised_}.load(std::memory_order_acquire) !=
               static_cast<std::int32_t>(cancellation_state_t::wanted_k);
    }

  private:

    /** A @c cancellation_state_t, moved by @c raise alone. */
    std::int32_t raised_ = 0;
};

/** Which compactor @p placement asks for, given @p devices devices and a merge of @p key_slots_count key slots. */
[[nodiscard]] constexpr compactor_choice_t choose_compactor(compaction_placement_t placement, std::size_t devices,
                                                            std::uint64_t key_slots_count) noexcept {
    if (placement == compaction_placement_t::host_k) return compactor_choice_t::host_k;
    if (placement == compaction_placement_t::device_k) return compactor_choice_t::device_k;
    return devices != 0 && key_slots_count > storage_limits_t::cuda_compaction_threshold_k
               ? compactor_choice_t::device_k
               : compactor_choice_t::host_k;
}

/** The engine's status for the @p status a compactor reported. */
inline status_t compaction_status(st::status_t status) noexcept {
    switch (status) {
    case st::status_t::success_k: return {};
    case st::status_t::out_of_memory_heap_k: return {status_code_t::out_of_memory_k, "A compaction could not grow"};
    case st::status_t::invalid_argument_k:
        return {status_code_t::invalid_argument_k, "A compaction refused its bounds"};
    case st::status_t::capacity_exhausted_k:
        return {status_code_t::capacity_exhausted_k, "A compaction ran out of room"};
    case st::status_t::input_output_k: return {status_code_t::io_k, "A compaction could not move its key slots"};
    default: return {status_code_t::device_k, "A compaction failed on the device it ran on"};
    }
}

/** Drains a channel's queue pair before the pipelines above it hand their blocks back, which a failure needs too. */
template <typename disk_type_>
    requires ring_concept<disk_type_>
class channel_drain {
  public:

    channel_drain(disk_type_& disk, io_channel_t& channel) noexcept : disk_(disk), channel_(channel) {}
    channel_drain(channel_drain const&) = delete;
    channel_drain& operator=(channel_drain const&) = delete;

    ~channel_drain() noexcept { [[maybe_unused]] status_t const drained = disk_.drain(channel_.queue_pair()); }

  private:

    /** The ring the queue pair belongs to. */
    disk_type_& disk_;

    /** The channel whose queue pair is drained. */
    io_channel_t& channel_;
};

#pragma endregion Vocabulary

#pragma region Merger

/**
 *  @brief Merges one level into the level below it, writing a whole new sorted level.
 *
 *  Both source levels are consumed whole rather than by the ranges they overlap, so the runs the new
 *  level holds are the only survivors and every source run is free the moment the merge is published.
 *
 *  @tparam atomic_reference_ The reference a caller's cancellation word is read through.
 *  @tparam waiter_type_ What the core does while the merge waits for its reads and barriers.
 */
template <typename key_slot_type_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
class lsm_level_merger {
  public:

#pragma region Types

    /** The key slot the runs of both levels hold. */
    using key_slot_t = key_slot_type_;

    /** The volume the runs live on. */
    using volume_t = volume_type_;

    /** The level this merge reads and writes. */
    using level_t = lsm_level<key_slot_t, volume_t, waiter_type_>;

    /** One immutable run of a level. */
    using sorted_run_t = typename level_t::sorted_run_t;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The transport the volume moves bytes through. */
    using disk_t = std::remove_reference_t<decltype(std::declval<volume_type_&>().disk())>;

    /** The word a caller raises to stop a merge. */
    using cancellation_t = compaction_cancellation<atomic_reference_>;

    /** Where the sections of a run sit inside its blob. */
    using sorted_run_layout_t = sorted_run_layout<key_slot_t>;

    /** The pipeline the gather reads its source values through. */
    using read_pipeline_t = basic_read_pipeline<disk_t>;

    /** The pipeline every section of every new run is written through. */
    using write_pipeline_t = basic_write_pipeline<disk_t, waiter_type_>;

    /** What one compaction answers: the new runs, their cells and where their values come from. */
    using compaction_output_t = compaction_output<key_slot_t>;

    /** How the values of these runs are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

    /** Whether the values sit in the key slots themselves, so a merge moves no value bytes. */
    static constexpr bool holds_values_inline_k = is_inline(value_layout_k);

    /** What one merge is asked to do, beyond the volume and the channels it is handed. */
    struct merge_request_t {

        /** The level whose runs shadow the lower level's, which is never null and never empty. */
        level_t const* upper = nullptr;

        /** The level the upper one merges into, or null when the merge writes a level that holds nothing yet. */
        level_t const* lower = nullptr;

        /** The number the new level takes. */
        std::uint32_t number = 0;

        /** Whether anything sits below the new level, which decides whether remove deltas drop. */
        lsm_level_depth_t depth = lsm_level_depth_t::intermediate_k;

        /** The largest value the collection admits, which every new run's header carries. */
        stored_value_size_t value_max_size = 0;

        /** Most key slots one new run takes, which bounds how far a binary search inside it walks. */
        std::uint64_t run_key_slots_capacity = storage_limits_t::memory_runs_max_deltas_k;

        /** Largest new run in bytes, which a run of one key slot may still exceed. */
        std::uint64_t run_bytes_limit = storage_limits_t::sorted_run_file_max_size_k;

        /** Where the compaction runs. */
        compaction_placement_t placement = compaction_placement_t::automatic_k;

        /** The devices the executor named, the first of which a device compaction runs on, empty for the host alone. */
        std::span<int const> devices;

        /** The word the merge reads between blocks, or null for a merge nobody stops. */
        cancellation_t const* cancellation = nullptr;

        /** Which older versions survive for readers and for a mount, and the floor the new runs count from. */
        compaction_stamps_t stamps;
    };

#pragma endregion Types

#pragma region Merging

    /**
     *  @brief Merges @p request.upper into @p request.lower and writes the survivors as a new sorted level.
     *  @param[inout] volume The volume the new runs are reserved on, which every source run also lives on.
     *  @param[inout] reading_channel The channel the source values are gathered on, held by the calling thread.
     *  @param[inout] writing_channel The channel the new runs are written on, which must not be @p reading_channel.
     *  @param[in] request Which levels merge, what the new runs may grow to, and where the compaction runs.
     *  @param[in] fold How the collection's changes compose and apply, which reads their values on
     *      @p reading_channel before any is gathered.
     *  @return The new level, or the status that stopped the merge with every new run already dropped.
     */
    template <utilities::fold_concept fold_type_>
    static expected<level_t> merge(volume_t& volume, io_channel_t& reading_channel, io_channel_t& writing_channel,
                                   merge_request_t const& request, fold_type_ const& fold) noexcept {
        if (request.upper == nullptr)
            return status_t {status_code_t::invalid_argument_k, "A merge reads an upper level"};
        if (&reading_channel == &writing_channel)
            return status_t {status_code_t::invalid_argument_k, "A merge reads and writes on two channels"};

        st::basic_vector<sorted_run_t const*> sources;
        st::basic_vector<sorted_run_index_view<key_slot_t>> views;
        if (status_t const pointed = point_views_(*request.upper, request.lower, sources, views); pointed.failed())
            return pointed;

        // A fold reads the values of the changes it folds, from their runs, one at a time.
        auto read_value = [&](located_key_slot<key_slot_t> const& record, utilities::arena_allocator<byte_t>& arena,
                              std::span<byte_t const>& bytes) noexcept {
            std::size_t const size = record.key_slot.size;
            byte_t* const into = size != 0 ? arena.allocate(size) : nullptr;
            if (size != 0 && into == nullptr) return st::status_t::out_of_memory_heap_k;
            if (sources[record.sorted_run]->read_value(reading_channel, record.key_slot, {into, size}).failed())
                return st::status_t::input_output_k;
            bytes = {into, size};
            return st::status_t::success_k;
        };

        std::size_t const upper_runs = request.upper->size();
        std::span<sorted_run_index_view<key_slot_t> const> const all {views};
        compaction_output_t output;
        if (status_t const compacted = run_compaction_(
                kind_of_(request.upper->order(), request.depth), all.first(upper_runs), all.subspan(upper_runs),
                bounds_of_(request), request.stamps, request.placement, request.devices, fold, read_value, output);
            compacted.failed())
            return compacted;

        level_t merged = level_t::of(request.number, lsm_level_order_t::sorted_k);
        if (status_t const room = merged.reserve(output.sorted_runs.size()); room.failed()) return room;

        std::uint64_t first_cell = 0;
        std::uint64_t first_key_slot = 0;
        for (std::size_t index = 0; index != output.sorted_runs.size(); ++index) {
            planned_run_t planned;
            planned.key_slots_count = output.sorted_runs[index].key_slots_count;
            planned.stored_values_bytes = output.sorted_runs[index].stored_values_bytes;
            planned.first_cell = first_cell;
            planned.first_key_slot = first_key_slot;

            auto [written, write_run_status] = write_run_(
                volume, reading_channel, writing_channel, std::span<sorted_run_t const* const> {sources}, output,
                planned, request.value_max_size, request.stamps.floor, request.cancellation);
            if (write_run_status.failed()) {
                [[maybe_unused]] status_t const abandoned = merged.drop(volume, writing_channel);
                return write_run_status;
            }
            if (status_t const adopted = merged.bind_queue_pair(std::move(written)); adopted.failed()) {
                [[maybe_unused]] status_t const abandoned = merged.drop(volume, writing_channel);
                return adopted;
            }
            first_cell += round_up_to_multiple(planned.key_slots_count, sorted_run_layout_t::branching_k);
            first_key_slot += planned.key_slots_count;
        }
        return merged;
    }

#pragma endregion Merging

  private:

#pragma region Private Types

    /** One run of the compaction's plan, with where its cells and its migrations start in the output. */
    struct planned_run_t {

        /** Key slots the run holds, padding cells excluded. */
        std::uint64_t key_slots_count = 0;

        /** Bytes of the values its key slots locate, before the section's alignment padding. */
        std::uint64_t stored_values_bytes = 0;

        /** Where this run's cells start in the compaction's cells. */
        std::uint64_t first_cell = 0;

        /** Where this run's survivors start in the compaction's migrations. */
        std::uint64_t first_key_slot = 0;
    };

    /** One block of one source value's read, kept until its completion carries the block back. */
    struct pending_piece_t {

        /** Where in the gather's scratch the bytes of this block land. */
        std::uint64_t destination = 0;

        /** Bytes of the block before the value's own bytes. */
        std::uint32_t skipped = 0;

        /** Bytes of the value this block carries. */
        std::uint32_t carried = 0;
    };

#pragma endregion Private Types

#pragma region Private Methods

    /** Which compaction the order of the upper level and the depth of the new level together name. */
    [[nodiscard]] static constexpr compaction_kind_t kind_of_(lsm_level_order_t order,
                                                              lsm_level_depth_t depth) noexcept {
        if (order == lsm_level_order_t::overlapping_k)
            return depth == lsm_level_depth_t::bottom_k ? compaction_kind_t::overlapping_into_bottom_k
                                                        : compaction_kind_t::overlapping_into_intermediate_k;
        return depth == lsm_level_depth_t::bottom_k ? compaction_kind_t::sorted_into_bottom_k
                                                    : compaction_kind_t::sorted_into_intermediate_k;
    }

    /** The bounds every run the merge writes must fit, which the run layout and @p request together fix. */
    [[nodiscard]] static sorted_run_bounds_t bounds_of_(merge_request_t const& request) noexcept {
        sorted_run_bounds_t bounds;
        bounds.key_slots_capacity = request.run_key_slots_capacity;
        bounds.file_bytes_limit = request.run_bytes_limit;
        bounds.header_bytes = sorted_run_layout_t::header_bytes();
        bounds.alignment_bytes = portable_alignment_k;
        bounds.branching = sorted_run_layout_t::branching_k;
        return bounds;
    }

    /** Points a view of @p views at every source run's own cells, which the compactor reads in place. */
    static status_t point_views_(level_t const& upper, level_t const* lower,
                                 st::basic_vector<sorted_run_t const*>& sources,
                                 st::basic_vector<sorted_run_index_view<key_slot_t>>& views) noexcept {
        std::size_t const lower_runs = lower != nullptr ? lower->size() : 0;
        std::size_t const runs = upper.size() + lower_runs;
        if (sources.resize(runs) != st::status_t::success_k || views.resize(runs) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A merge could not track its source runs"};
        for (std::size_t index = 0; index != upper.size(); ++index) sources[index] = &upper.run(index);
        for (std::size_t index = 0; index != lower_runs; ++index) sources[upper.size() + index] = &lower->run(index);

        for (std::size_t index = 0; index != runs; ++index) {
            sorted_run_t const& run = *sources[index];
            std::span<key_slot_t const> const cells = run.cells();
            if (run.layout().branching != sorted_run_layout_t::branching_k ||
                cells.size() != run.layout().cells_count())
                return {status_code_t::malformed_k, "A run's index is not laid out as a merge reads it"};
            views[index] = sorted_run_index_view<key_slot_t> {cells.data(), run.population_count(), &run.stamps(),
                                                              &run.kinds(), run.tails()};
        }
        return {};
    }

#if USTORE_WITH_CUDA

    /** Compacts on the first of @p devices when @p placement asks for one and no run holds a change, which only the
     *  host folds, and on the calling thread otherwise, where runs carrying tails always merge. */
    template <typename fold_type_, typename value_reader_type_>
    static status_t run_compaction_(compaction_kind_t kind, std::span<sorted_run_index_view<key_slot_t> const> upper,
                                    std::span<sorted_run_index_view<key_slot_t> const> lower,
                                    sorted_run_bounds_t const& bounds, compaction_stamps_t const& stamps,
                                    compaction_placement_t placement, std::span<int const> devices,
                                    fold_type_ const& fold, value_reader_type_&& read_value,
                                    compaction_output_t& output) noexcept {
        // No device compaction is compiled for key slots carrying tails.
        if constexpr (!tailed_key_slot_concept<key_slot_t>) {
            std::uint64_t merged = 0;
            bool changes = false;
            for (std::size_t index = 0; index != upper.size(); ++index) {
                merged += upper[index].key_slots_count;
                changes |= holds_changes(upper[index].kinds);
            }
            for (std::size_t index = 0; index != lower.size(); ++index) {
                merged += lower[index].key_slots_count;
                changes |= holds_changes(lower[index].kinds);
            }

            if (!changes && choose_compactor(placement, devices.size(), merged) == compactor_choice_t::device_k) {
                if (devices.empty()) return {status_code_t::device_k, "A device compaction names no device"};
                st::expected<cuda_compactor_t> compactor = cuda_compactor_t::make(devices.front());
                if (!compactor) return compaction_status(compactor.status());
                return compaction_status(compactor->compact(kind, upper, lower, bounds, stamps, output));
            }
        }
        return compaction_status(
            host_compactor_t {}.compact(kind, upper, lower, bounds, stamps, fold, read_value, output));
    }

#else

    /** Compacts on the calling thread, which is the only compactor a build without CUDA carries. */
    template <typename fold_type_, typename value_reader_type_>
    static status_t run_compaction_(compaction_kind_t kind, std::span<sorted_run_index_view<key_slot_t> const> upper,
                                    std::span<sorted_run_index_view<key_slot_t> const> lower,
                                    sorted_run_bounds_t const& bounds, compaction_stamps_t const& stamps,
                                    compaction_placement_t placement, std::span<int const>, fold_type_ const& fold,
                                    value_reader_type_&& read_value, compaction_output_t& output) noexcept {
        if (choose_compactor(placement, 0, 0) == compactor_choice_t::device_k)
            return {status_code_t::unsupported_k, "This build carries no device compactor"};
        return compaction_status(
            host_compactor_t {}.compact(kind, upper, lower, bounds, stamps, fold, read_value, output));
    }

#endif

    /** Takes a streaming block from @p writer, polling its completions when every block is out. */
    static expected<pooled_buffer_t> acquire_buffer_or_poll_(write_pipeline_t& writer) noexcept {
        expected<pooled_buffer_t> block = writer.acquire_stream_buffer();
        while (block == status_code_t::capacity_exhausted_k && writer.outstanding() != 0) {
            if (status_t const flushed = writer.submit(); flushed.failed()) return flushed;
            if (status_t const polled = writer.poll_one_completion(); polled.failed()) return polled;
            block = writer.acquire_stream_buffer();
        }
        return block;
    }

    /** Whether @p cancellation has been raised, which every block boundary of a merge asks. */
    [[nodiscard]] static bool stopped_(cancellation_t const* cancellation) noexcept {
        return cancellation != nullptr && cancellation->cancelled();
    }

    /** The status a merge answers once its caller has cancelled it. */
    static status_t cancelled_status_() noexcept {
        return {status_code_t::uninitialized_k, "The caller cancelled this compaction"};
    }

    /** Reserves a blob for the run @p planned describes, writes its three sections, and opens it. */
    static expected<sorted_run_t> write_run_(volume_t& volume, io_channel_t& reading_channel,
                                             io_channel_t& writing_channel,
                                             std::span<sorted_run_t const* const> sources,
                                             compaction_output_t const& output, planned_run_t const& planned,
                                             stored_value_size_t value_max_size, std::uint64_t floor,
                                             cancellation_t const* cancellation) noexcept {
        uuid_t const content_id = uuid_generator();
        if (!content_id.is_valid())
            return status_t {status_code_t::internal_k, "A merge could not name the run it writes"};

        // The compactor already raised every stamp to the floor, so the column packs them as they are.
        std::uint64_t newest = floor;
        auto [column, pack_status] = pack_stamps(
            std::span<std::uint64_t const> {output.stamps.data() + planned.first_key_slot,
                                            static_cast<std::size_t>(planned.key_slots_count)},
            floor, newest);
        if (pack_status.failed()) return pack_status;
        auto [kinds, pack_kinds_status] = pack_kinds(std::span<std::uint8_t const> {
            output.kinds.data() + planned.first_key_slot, static_cast<std::size_t>(planned.key_slots_count)});
        if (pack_kinds_status.failed()) return pack_kinds_status;

        sorted_run_header_t header {};
        header.magic = sorted_run_magic_k;
        header.format_version = sorted_run_format_version_k;
        header.key_slot_bytes = static_cast<std::uint32_t>(sizeof(key_slot_t));
        header.key_slots_count = planned.key_slots_count;
        header.stored_values_bytes = holds_values_inline_k ? 0 : planned.stored_values_bytes;
        header.value_max_size = value_max_size;
        header.branching = static_cast<std::uint16_t>(sorted_run_layout_t::branching_k);
        header.value_layout = value_layout_k;
        header.content_id = content_id;
        header.stamps_bytes = column.saved_bytes();
        header.stamps_floor = floor;
        header.newest_stamp = newest;
        header.kinds_bytes = kinds.saved_bytes();

        // The run's own tails and their offsets from its first key slot on, none for a run of whole keys.
        st::basic_vector<std::uint32_t> tail_offsets;
        std::span<byte_t const> tails;
        if (!output.tail_offsets.empty()) {
            std::uint64_t const begin = output.tail_offsets[planned.first_key_slot];
            std::uint64_t const end = output.tail_offsets[planned.first_key_slot + planned.key_slots_count];
            if (end - begin > std::numeric_limits<std::uint32_t>::max())
                return status_t {status_code_t::capacity_exhausted_k, "A merged run's tails outgrow their offsets"};
            if (end != begin) {
                if (tail_offsets.resize(static_cast<std::size_t>(planned.key_slots_count + 1)) !=
                    st::status_t::success_k)
                    return status_t {status_code_t::out_of_memory_k, "A merge could not lay out a run's tails"};
                for (std::uint64_t index = 0; index != tail_offsets.size(); ++index)
                    tail_offsets[index] = static_cast<std::uint32_t>(
                        output.tail_offsets[planned.first_key_slot + index] - begin);
                tails = std::span<byte_t const> {output.tails.data() + begin, static_cast<std::size_t>(end - begin)};
            }
        }
        header.key_tails_bytes = tails.size();

        std::uint64_t const reserved_bytes = sorted_run_layout_t::blob_bytes(
            header.key_slots_count, header.stored_values_bytes, header.stamps_bytes, header.kinds_bytes,
            header.key_tails_bytes);
        auto [reserved, create_status] = volume.create(sorted_run_t::name_of(content_id), blob_kind_t::sorted_run_k,
                                                       reserved_bytes);
        if (create_status.failed()) return create_status;
        volume_blob_t const blob = reserved;

        key_tails_column_t const run_tails {std::span<std::uint32_t const> {tail_offsets}, tails};
        if (status_t const written = write_sections_(volume, reading_channel, writing_channel, sources, output, planned,
                                                     header, column, kinds, run_tails, blob, cancellation);
            written.failed()) {
            [[maybe_unused]] status_t const abandoned = volume.drop(writing_channel, sorted_run_t::name_of(content_id));
            return written;
        }
        if (status_t const sealed = volume.seal(writing_channel, blob); sealed.failed()) {
            [[maybe_unused]] status_t const abandoned = volume.drop(writing_channel, sorted_run_t::name_of(content_id));
            return sealed;
        }
        return sorted_run_t::attach(volume, writing_channel, blob);
    }

    /** Writes the header, the cells, the gathered values, the tails, the stamps, the kinds and the tail offsets of one
     *  new run, and makes them all durable. */
    static status_t write_sections_(volume_t& volume, io_channel_t& reading_channel, io_channel_t& writing_channel,
                                    std::span<sorted_run_t const* const> sources, compaction_output_t const& output,
                                    planned_run_t const& planned, sorted_run_header_t const& header,
                                    stamps_column_t const& column, kinds_column_t const& kinds,
                                    key_tails_column_t const& tails, volume_blob_t const& blob,
                                    cancellation_t const* cancellation) noexcept {
        std::uint64_t const block_bytes = round_down_to_multiple(writing_channel.streams().buffer_bytes(),
                                                                 portable_alignment_k);
        if (block_bytes < sorted_run_layout_t::header_bytes())
            return {status_code_t::capacity_exhausted_k, "A channel's streaming block is shorter than a run's header"};

        write_pipeline_t writer {volume.disk(), writing_channel, blob.medium};
        channel_drain<disk_t> const parked {volume.disk(), writing_channel};
        if (stopped_(cancellation)) return cancelled_status_();

        std::uint64_t const head_bytes = sorted_run_layout_t::header_bytes();
        auto [head, acquire_buffer_or_poll_status] = acquire_buffer_or_poll_(writer);
        if (acquire_buffer_or_poll_status.failed()) return acquire_buffer_or_poll_status;
        std::memset(head.bytes().data(), 0, static_cast<std::size_t>(head_bytes));
        std::memcpy(head.bytes().data(), &header, sizeof(header));
        if (status_t const queued = writer.enqueue_write(std::move(head), blob.slice(0, head_bytes)); queued.failed())
            return queued;
        if (status_t const flushed = writer.submit(); flushed.failed()) return flushed;

        if (status_t const cells = write_cells_(writer, output, planned, header, blob, block_bytes); cells.failed())
            return cells;
        if (status_t const values = write_values_(volume, reading_channel, writer, sources, output, planned, header,
                                                  blob, block_bytes, cancellation);
            values.failed())
            return values;
        if (status_t const written = sorted_run_t::write_tails(writer, blob, header, tails.bytes); written.failed())
            return written;
        if (status_t const trailer = sorted_run_t::write_trailer(writer, blob, header, column, kinds, tails.offsets);
            trailer.failed())
            return trailer;
        return writer.make_durable();
    }

    /** Writes the cells the compaction laid out, which are already in cell order with their padding. */
    static status_t write_cells_(write_pipeline_t& writer, compaction_output_t const& output,
                                 planned_run_t const& planned, sorted_run_header_t const& header,
                                 volume_blob_t const& blob, std::uint64_t block_bytes) noexcept {
        std::uint64_t const section = sorted_run_layout_t::index_section_bytes(header.key_slots_count);
        std::uint64_t const filled = sorted_run_layout_t::index_bytes(header.key_slots_count);
        byte_t const* const cells = reinterpret_cast<byte_t const*>(output.cells.data() + planned.first_cell);
        for (std::uint64_t written = 0; written != section;) {
            std::uint64_t const room = section - written;
            std::uint64_t const chunk = block_bytes < room ? block_bytes : room;
            auto [block, acquire_buffer_or_poll_status] = acquire_buffer_or_poll_(writer);
            if (acquire_buffer_or_poll_status.failed()) return acquire_buffer_or_poll_status;
            std::span<byte_t> const bytes = block.bytes().first(static_cast<std::size_t>(chunk));
            std::uint64_t const left = filled > written ? filled - written : 0;
            std::uint64_t const carried = left < chunk ? left : chunk;
            if (carried != 0) std::memcpy(bytes.data(), cells + written, static_cast<std::size_t>(carried));
            std::memset(bytes.data() + carried, 0, static_cast<std::size_t>(chunk - carried));
            if (status_t const queued = writer.enqueue_write(
                    std::move(block), blob.slice(sorted_run_layout_t::index_offset() + written, chunk));
                queued.failed())
                return queued;
            if (status_t const flushed = writer.submit(); flushed.failed()) return flushed;
            written += chunk;
        }
        return {};
    }

    /**
     *  @brief Gathers every survivor's value out of its old run and writes them back to back.
     *
     *  @note Values are read into a scratch buffer a block deep plus one read block, so a value that
     *      straddles the boundary between two written blocks is still read whole exactly once.
     */
    static status_t write_values_(volume_t& volume, io_channel_t& reading_channel, write_pipeline_t& writer,
                                  std::span<sorted_run_t const* const> sources, compaction_output_t const& output,
                                  planned_run_t const& planned, sorted_run_header_t const& header,
                                  volume_blob_t const& blob, std::uint64_t block_bytes,
                                  cancellation_t const* cancellation) noexcept {
        if constexpr (holds_values_inline_k) return {};
        else {
            std::uint64_t const section = sorted_run_layout_t::values_section_bytes(header.stored_values_bytes);
            if (section == 0) return {};
            std::uint64_t const values_offset = sorted_run_layout_t::values_offset(header.key_slots_count);
            std::uint64_t const read_block = round_down_to_multiple(reading_channel.requests().buffer_bytes(),
                                                                    portable_alignment_k);
            if (read_block == 0)
                return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than one page"};

            std::size_t const depth = reading_channel.queue_pair().depth();
            st::basic_vector<pending_piece_t> pending;
            if (pending.resize(depth) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A merge could not track its reads in flight"};
            st::basic_vector<byte_t> scratch;
            if (scratch.resize(static_cast<std::size_t>(block_bytes + read_block)) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A merge could not hold the values it gathers"};

            read_pipeline_t reader {volume.disk(), reading_channel, volume.medium()};
            channel_drain<disk_t> const parked {volume.disk(), reading_channel};

            status_t first;
            auto sink = [&](completed_buffer_t const& completed) noexcept {
                pending_piece_t const& piece = pending[completed.tag % depth];
                if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
                    if (first.succeeded()) first = whole;
                    return;
                }
                if (piece.carried == 0) return;
                std::memcpy(scratch.data() + piece.destination, completed.bytes.data() + piece.skipped, piece.carried);
            };

            std::uint64_t survivor = planned.first_key_slot;
            std::uint64_t const survivors_end = planned.first_key_slot + planned.key_slots_count;
            std::uint64_t chunk_offset = 0;
            std::uint64_t gathered = 0;
            std::uint64_t written = 0;
            request_tag_t tag = 0;
            while (written != section) {
                if (stopped_(cancellation)) {
                    volume.disk().cancel(reading_channel.queue_pair());
                    return cancelled_status_();
                }

                while (gathered < block_bytes && survivor != survivors_end) {
                    while (gathered < block_bytes && survivor != survivors_end && reader.can_enqueue()) {
                        stored_value_migration_t const& migration = output.migrations[survivor];
                        if (migration.size == 0) {
                            ++survivor;
                            chunk_offset = 0;
                            continue;
                        }
                        // A value a fold wrote is copied out of the compaction's own bytes, a read block at a time.
                        if (migration.old_sorted_run == sources.size()) {
                            std::uint64_t const length = std::min<std::uint64_t>(read_block,
                                                                                 migration.size - chunk_offset);
                            std::memcpy(scratch.data() + gathered,
                                        output.folded.data() + migration.old_location + chunk_offset,
                                        static_cast<std::size_t>(length));
                            gathered += length;
                            chunk_offset += length;
                            if (chunk_offset == migration.size) {
                                ++survivor;
                                chunk_offset = 0;
                            }
                            continue;
                        }
                        std::uint64_t const aligned = round_down_to_multiple(migration.old_location,
                                                                             portable_alignment_k);
                        std::uint64_t const skipped = migration.old_location - aligned;
                        std::uint64_t const covered = round_up_to_multiple(skipped + migration.size,
                                                                           portable_alignment_k);
                        extent_t const window = sources[migration.old_sorted_run]->blob().slice(aligned, covered);
                        if (window.length != covered)
                            return {status_code_t::malformed_k, "A source run is shorter than the value it locates"};

                        std::uint64_t const length = read_block < covered - chunk_offset ? read_block
                                                                                         : covered - chunk_offset;
                        std::uint64_t const from = chunk_offset > skipped ? chunk_offset : skipped;
                        std::uint64_t const wanted_last = skipped + migration.size;
                        std::uint64_t const to = chunk_offset + length < wanted_last ? chunk_offset + length
                                                                                     : wanted_last;
                        pending_piece_t piece;
                        piece.destination = gathered;
                        piece.skipped = static_cast<std::uint32_t>(from - chunk_offset);
                        piece.carried = static_cast<std::uint32_t>(to > from ? to - from : 0);
                        pending[tag % depth] = piece;
                        if (reader.enqueue_read(extent_t {window.offset + chunk_offset, length}, tag).failed()) break;
                        ++tag;
                        gathered += piece.carried;
                        chunk_offset += length;
                        if (chunk_offset == covered) {
                            ++survivor;
                            chunk_offset = 0;
                        }
                    }
                    if (status_t const flushed = reader.submit(); flushed.failed()) return flushed;
                    auto [polled, poll_completions_status] = reader.poll_completions(sink);
                    if (poll_completions_status.failed()) return poll_completions_status;
                    if (first.failed()) return first;
                }
                if (status_t const drained = reader.poll_all_completions(sink); drained.failed()) return drained;
                if (first.failed()) return first;

                std::uint64_t const room = section - written;
                std::uint64_t const chunk = block_bytes < room ? block_bytes : room;
                std::uint64_t const carried = gathered < chunk ? gathered : chunk;
                auto [block, acquire_buffer_or_poll_status] = acquire_buffer_or_poll_(writer);
                if (acquire_buffer_or_poll_status.failed()) return acquire_buffer_or_poll_status;
                std::span<byte_t> const bytes = block.bytes().first(static_cast<std::size_t>(chunk));
                if (carried != 0) std::memcpy(bytes.data(), scratch.data(), static_cast<std::size_t>(carried));
                std::memset(bytes.data() + carried, 0, static_cast<std::size_t>(chunk - carried));
                if (status_t const queued = writer.enqueue_write(std::move(block),
                                                                 blob.slice(values_offset + written, chunk));
                    queued.failed())
                    return queued;
                if (status_t const flushed = writer.submit(); flushed.failed()) return flushed;
                written += chunk;
                if (gathered > carried)
                    std::memmove(scratch.data(), scratch.data() + carried,
                                 static_cast<std::size_t>(gathered - carried));
                gathered -= carried;
            }
            return first;
        }
    }

#pragma endregion Private Methods
};

#pragma endregion Merger

#pragma endregion LSM Level Merger

#pragma region LSM Tree

#pragma region Vocabulary

/** Where one key sits in a tree, which is the level that answered and where inside that level. */
struct lsm_tree_position_t {

    /** The level that answered, which is also its index in the snapshot the lookup ran against. */
    std::uint32_t level_number = 0;

    /** Where inside that level the key sits. */
    lsm_level_position_t level_position;
};

/** What one pass of compaction did. */
enum class compaction_outcome_t : std::uint8_t {

    /** Every level was inside its bounds, or another thread was already compacting. */
    nothing_to_compact_k,

    /** A merge ran and the level set it produced is live. */
    published_k,
};

/** One run of a tree as a schema names it, which is everything a restore needs to open it again. */
struct lsm_tree_run_record_t {

    /** The level the run belongs to, zero being the level flushes land on. */
    std::uint32_t level_number = 0;

    /** The run's own identifier, which is also the name its blob answers to on the volume. */
    uuid_t content_id {};
};

/**
 *  @brief What a compaction calls once its new level set is live and before it reclaims anything.
 *
 *  A durable record of the set a tree now holds must precede the drop of a run that set no longer
 *  names, or a crash between the two leaves a record naming bytes the volume has already handed out.
 */
struct lsm_tree_publication_hook_t {

    /** Called with @c context once the new set is built and before it is made current, or null; a failure abandons
     *  the set and every run only it names, leaving the current set as it was. */
    status_t (*prepare)(void* context) noexcept = nullptr;

    /** Called with @c context after the publication and before the first consumed run is dropped. */
    status_t (*notify)(void* context) noexcept = nullptr;

    /** What @c notify is handed, which is the owner that records the run set the tree now holds. */
    void* context = nullptr;
};

#pragma endregion Vocabulary

#pragma region Tree

/**
 *  @brief Every level of one collection's LSM tree, as the level set its writer last published.
 *
 *  Readers never reach the tree's own pointer: each publication hands the new set to a hook that publishes it, and
 *  the set is the only allocation a reader walks, so moving the tree moves no set.
 *
 *  @tparam atomic_reference_ The reference the compaction claim and a caller's cancellation word are owned through.
 *  @tparam waiter_type_ What the core does while the tree waits for its reads and barriers.
 */
template <typename key_slot_type_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
class lsm_tree {
  public:

#pragma region Types

    /** The key slot every run of this tree holds. */
    using key_slot_t = key_slot_type_;

    /** The volume every run of this tree lives on. */
    using volume_t = volume_type_;

    /** One level of the tree. */
    using level_t = lsm_level<key_slot_t, volume_t, waiter_type_>;

    /** One immutable run of a level. */
    using sorted_run_t = typename level_t::sorted_run_t;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The transport the volume moves bytes through. */
    using disk_t = std::remove_reference_t<decltype(std::declval<volume_type_&>().disk())>;

    /** The compaction that replaces two levels with one. */
    using merger_t = lsm_level_merger<key_slot_t, volume_t, atomic_reference_, waiter_type_>;

    /** The word a caller raises to stop a merge. */
    using cancellation_t = compaction_cancellation<atomic_reference_>;

    /** The pipeline a batched read refills. */
    using read_pipeline_t = basic_read_pipeline<disk_t>;

    /** How the values of this tree are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

    /** Whether the values sit in the key slots themselves, so reading one needs no I/O. */
    static constexpr bool holds_values_inline_k = is_inline(value_layout_k);

    /** Every level in order of its number, which nothing changes once a reader may hold it. */
    class level_set_t {
      public:

        level_set_t() noexcept = default;
        level_set_t(level_set_t const&) = delete;
        level_set_t& operator=(level_set_t const&) = delete;

        /** Levels the set holds, which is at least one. */
        [[nodiscard]] std::size_t size() const noexcept { return levels_.size(); }

        /** The level numbered @p number, which is also the level at that index. */
        [[nodiscard]] level_t const& level(std::size_t number) const noexcept { return levels_[number]; }

        /** Key slots every level holds together, one key counted once per run that holds it. */
        [[nodiscard]] std::uint64_t population_count() const noexcept {
            std::uint64_t counted = 0;
            for (std::size_t number = 0; number != size(); ++number) counted += level(number).population_count();
            return counted;
        }

        /** Bytes every run of every level takes on its volume. */
        [[nodiscard]] std::uint64_t size_bytes() const noexcept {
            std::uint64_t counted = 0;
            for (std::size_t number = 0; number != size(); ++number) counted += level(number).size_bytes();
            return counted;
        }

        /** The newest stamp any run carries, which a mount resumes its stamps above. */
        [[nodiscard]] std::uint64_t newest_stamp() const noexcept {
            std::uint64_t newest = 0;
            for (std::size_t number = 0; number != size(); ++number)
                for (std::size_t index = 0; index != level(number).size(); ++index)
                    if (level(number).run(index).newest_stamp() > newest)
                        newest = level(number).run(index).newest_stamp();
            return newest;
        }

      private:

        friend class lsm_tree;

        /** The levels, which own their runs unless the set relinquishes them to its successor. */
        st::basic_vector<level_t> levels_;
    };

#pragma endregion Types

#pragma region Construction

    lsm_tree() noexcept = default;
    lsm_tree(lsm_tree const&) = delete;
    lsm_tree& operator=(lsm_tree const&) = delete;
    lsm_tree(lsm_tree&&) noexcept = default;
    lsm_tree& operator=(lsm_tree&&) noexcept = default;

    /** An empty tree of one level zero over @p volume, which must outlive it, flushed memory runs of up to
     *  @p memory_runs_max_bytes at a time. */
    static expected<lsm_tree> mount(volume_t& volume, lsm_tree_config_t const& config,
                                    std::size_t memory_runs_max_bytes) noexcept {
        // The enlarge factor sizes every level and level zero's run count alike, so a tree without one has no shape
        if (config.level_enlarge_factor < storage_limits_t::level_min_enlarge_factor_k ||
            config.level_enlarge_factor > storage_limits_t::level_max_enlarge_factor_k)
            return status_t {status_code_t::invalid_argument_k, "A tree grows its levels by a factor in its bounds"};
        lsm_tree tree;
        tree.volume_ = &volume;
        tree.config_ = config;
        tree.memory_runs_max_bytes_ = memory_runs_max_bytes;
        tree.levels_.reset(new (std::nothrow) level_set_t {});
        if (!tree.levels_ || tree.levels_->levels_.push_back(level_t::of(0, lsm_level_order_t::overlapping_k)) !=
                                 st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A tree could not hold its first level"};
        return tree;
    }

    /** Returns every run of every level to the volume, which destroys the tree's data and which only a tree no
     *  reader reaches may. */
    status_t destroy(io_channel_t& channel) noexcept {
        status_t first;
        for (std::size_t number = 0; number != levels_->size(); ++number)
            if (status_t const dropped = levels_->levels_[number].drop(*volume_, channel);
                dropped.failed() && first.succeeded())
                first = dropped;
        return first;
    }

#pragma endregion Construction

#pragma region Lookups

    /** The level set the last publication made current, which its writer builds the next one from. */
    [[nodiscard]] level_set_t const& levels() const noexcept { return *levels_; }

    /**
     *  @brief Whether @p taken carries @p stored_key, and whether the key slot holding it locates a value.
     *  @param[in] taken The level set the lookup runs against.
     *  @param[in] stored_key The key to look for.
     *  @param[out] position Where the key sits, which is meaningless when the answer is @c missing_k.
     *  @param[in] stamp The stamp a reader is pinned at, which sees the newest version at or below it.
     */
    [[nodiscard]] lookup_result_t find(level_set_t const& taken, stored_key_t stored_key, lsm_tree_position_t& position,
                                       std::uint64_t stamp = limit<std::uint64_t>::max()) const noexcept {
        position = lsm_tree_position_t {};
        for (std::size_t number = 0; number != taken.size(); ++number) {
            lsm_level_position_t inside;
            lookup_result_t const found = taken.level(number).find(stored_key, stamp, inside);
            if (found == lookup_result_t::missing_k) continue;
            position = lsm_tree_position_t {static_cast<std::uint32_t>(number), inside};
            return found;
        }
        return lookup_result_t::missing_k;
    }

#pragma endregion Lookups

#pragma region Batched Reads

    /**
     *  @brief Looks @p stored_keys up in @p taken and reads their values, refilling the channel as reads retire.
     *  @param[inout] channel The channel the reads are submitted and completed on, held by the calling thread.
     *  @param[in] taken The level set every lookup runs against.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key was, which separates a miss from a removed value.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     *  @param[in] stamp The stamp a reader is pinned at, which sees the newest version at or below it.
     *  @return Success, or the first failure any of the blocks reported.
     *
     *  @note A value larger than one block arrives as several calls, each carrying the bytes of the value
     *      before it, and a key whose value is present and empty is reported once with no bytes.
     */
    template <typename sink_type_>
    status_t read_values(io_channel_t& channel, level_set_t const& taken, std::span<stored_key_t const> stored_keys,
                         std::span<lookup_result_t> results, sink_type_&& sink,
                         std::uint64_t stamp = limit<std::uint64_t>::max()) const noexcept {
        if (results.size() != stored_keys.size())
            return {status_code_t::invalid_argument_k, "A batched read answers one result per key"};

        if constexpr (holds_values_inline_k) {
            for (std::size_t request = 0; request != stored_keys.size(); ++request) {
                lsm_tree_position_t position;
                results[request] = find(taken, stored_keys[request], position, stamp);
                if (results[request] != lookup_result_t::written_k) continue;
                key_slot_t const& slot = taken.level(position.level_number).key_slot(position.level_position);
                sink(request, std::uint64_t {0}, std::span<byte_t const> {slot.data, slot.size});
            }
            return {};
        }
        else {
            std::uint64_t const block_bytes = round_down_to_multiple(channel.requests().buffer_bytes(),
                                                                     portable_alignment_k);
            if (block_bytes == 0)
                return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than one page"};

            std::size_t const depth = channel.queue_pair().depth();
            st::basic_vector<pending_read_t> pending;
            if (pending.resize(depth) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A tree could not track its reads in flight"};

            read_pipeline_t reader {volume_->disk(), channel, volume_->medium()};
            status_t first;
            auto adapter = [&](completed_buffer_t const& completed) noexcept {
                pending_read_t const& piece = pending[completed.tag % depth];
                if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
                    if (first.succeeded()) first = whole;
                    return;
                }
                sink(piece.request, piece.offset, completed.bytes.subspan(piece.skipped, piece.carried));
            };

            std::size_t request = 0;
            std::uint64_t chunk = 0;
            request_tag_t tag = 0;
            value_window_t window;
            while (request != stored_keys.size() || reader.outstanding() != 0) {
                while (request != stored_keys.size() && reader.can_enqueue()) {
                    if (chunk == 0) {
                        lsm_tree_position_t position;
                        lookup_result_t const found = find(taken, stored_keys[request], position, stamp);
                        results[request] = found;
                        window = found == lookup_result_t::written_k
                                     ? taken.level(position.level_number).value_window(position.level_position)
                                     : value_window_t {};
                        if (window.extent.length == 0) {
                            if (found == lookup_result_t::written_k)
                                sink(request, std::uint64_t {0}, std::span<byte_t const> {});
                            ++request;
                            continue;
                        }
                    }
                    std::uint64_t const at = chunk * block_bytes;
                    std::uint64_t const room = window.extent.length - at;
                    std::uint64_t const length = block_bytes < room ? block_bytes : room;
                    pending[tag % depth] = piece_of_(window, request, at, length);
                    if (reader.enqueue_read(extent_t {window.extent.offset + at, length}, tag).failed()) break;
                    ++tag;
                    if (at + length == window.extent.length) {
                        ++request;
                        chunk = 0;
                    }
                    else ++chunk;
                }
                if (status_t const flushed = reader.submit(); flushed.failed()) return flushed;
                auto [polled, poll_completions_status] = reader.poll_completions(adapter);
                if (poll_completions_status.failed()) return poll_completions_status;
            }
            return first;
        }
    }

#pragma endregion Batched Reads

#pragma region Publication

    /**
     *  @brief Writes @p key_slots and @p stored_values as one run and publishes it onto level zero.
     *  @param[inout] channel The channel the run is written on, held by the calling thread.
     *  @param[in] key_slots The key slots in ascending order of their keys, as a write buffer drained them.
     *  @param[in] stamps The stamp of every key slot, in the same order.
     *  @param[in] kinds The @c delta_kind_t of every key slot, in the same order.
     *  @param[in] floor The stamp every stamp is raised to at least, since no reader can tell those below it apart.
     *  @param[in] stored_values The values back to back, located from the run's own values offset.
     *  @param[in] value_max_size The largest value the collection admits, kept in the run's header.
     *  @param[in] published What to call once the new set is current, which returns once no reader holds the old one.
     *  @param[in] tails The tails of the keys the key slots' heads cut short, in the same order, none for whole keys.
     */
    status_t flush(io_channel_t& channel, std::span<key_slot_t const> key_slots, std::span<std::uint64_t const> stamps,
                   std::span<std::uint8_t const> kinds, std::uint64_t floor, std::span<byte_t const> stored_values,
                   stored_value_size_t value_max_size,
                   lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {},
                   key_tails_column_t const& tails = {}) noexcept {
        auto [written, write_sorted_run_status] = sorted_run_t::write(
            *volume_, channel, uuid_generator(), key_slots, stamps, kinds, floor, stored_values, value_max_size, tails);
        if (write_sorted_run_status.failed()) return write_sorted_run_status;
        uuid_t const content_id = written.content_id();

        std::size_t const nothing_consumed = levels_->size();
        auto [next, carried_status] = carried_(nothing_consumed, nothing_consumed);
        if (carried_status.failed()) {
            [[maybe_unused]] status_t const abandoned = sorted_run_t::drop(*volume_, channel, content_id);
            return carried_status;
        }
        if (status_t const adopted = next->levels_[0].bind_queue_pair(std::move(written)); adopted.failed()) {
            relinquish_(*next);
            [[maybe_unused]] status_t const abandoned = sorted_run_t::drop(*volume_, channel, content_id);
            return adopted;
        }
        return replace_(std::move(next), published, channel, {});
    }

    /**
     *  @brief Opens the runs @p runs names and publishes them as the level set of this tree.
     *  @param[inout] channel The channel every run's header and index is read on, held by the calling thread.
     *  @param[in] runs The runs in the order their levels hold them, which is oldest first on level zero.
     *  @param[in] published What to call once the restored set is current, as a flush does.
     *  @return Success, or the status that stopped it with the empty level set still current.
     *
     *  @note Only a tree still holding the one empty level @c mount built takes these, so a reopen
     *      adopts what the medium carries and no call ever merges two level sets into one.
     */
    status_t restore(io_channel_t& channel, std::span<lsm_tree_run_record_t const> runs,
                     lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {}) noexcept {
        if (levels_->size() != 1 || !levels_->level(0).empty())
            return {status_code_t::invalid_argument_k, "A tree takes its stored runs only while it holds none"};
        if (runs.empty()) return {};

        std::uint32_t deepest = 0;
        for (std::size_t index = 0; index != runs.size(); ++index)
            if (runs[index].level_number > deepest) deepest = runs[index].level_number;

        // The set owns every run it opens, so abandoning it frees them all.
        std::unique_ptr<level_set_t> next {new (std::nothrow) level_set_t {}};
        if (!next || next->levels_.reserve(std::size_t {deepest} + 1) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A tree could not hold the levels it restores"};
        for (std::uint32_t number = 0; number <= deepest; ++number) {
            level_t level = level_t::of(number,
                                        number == 0 ? lsm_level_order_t::overlapping_k : lsm_level_order_t::sorted_k);
            [[maybe_unused]] st::status_t const placed = next->levels_.push_back(std::move(level));
        }

        for (std::size_t index = 0; index != runs.size(); ++index) {
            auto [opened, open_sorted_run_status] = sorted_run_t::open(*volume_, channel, runs[index].content_id);
            if (open_sorted_run_status.failed()) return open_sorted_run_status;
            if (status_t const adopted = next->levels_[runs[index].level_number].bind_queue_pair(std::move(opened));
                adopted.failed())
                return adopted;
        }
        return replace_(std::move(next), published, channel, {});
    }

    /** Whether any level of @p taken is over the bytes its number admits. */
    [[nodiscard]] bool needs_compaction(level_set_t const& taken) const noexcept {
        return overfull_(taken) != taken.size();
    }

    /**
     *  @brief Merges the shallowest level that is over its bounds into the level below it and publishes it.
     *  @param[inout] reading_channel The channel the merge gathers source values on, held by the calling thread.
     *  @param[inout] writing_channel The channel the merge writes its new runs on, which is not @p reading_channel.
     *  @param[in] placement Where the compaction runs, which a collection leaves automatic.
     *  @param[in] devices The devices the executor named, the first of which a device compaction runs on.
     *  @param[in] cancellation The word the merge reads between blocks, or null for a merge nobody stops.
     *  @param[in] fold How the collection's changes compose and apply.
     *  @param[in] published What to call once the merged set is current, which returns once no reader holds the set
     *      it replaced and before a consumed run is dropped.
     *  @param[in] stamps Which older versions survive for readers and for a mount, and the floor the new runs count
     *      from, where the default keeps the newest version of every key alone.
     *  @return What the pass did, or the status that stopped it with the current level set untouched.
     */
    template <utilities::fold_concept fold_type_>
    expected<compaction_outcome_t> compact(
        io_channel_t& reading_channel, io_channel_t& writing_channel, compaction_placement_t placement,
        std::span<int const> devices, cancellation_t const* cancellation, fold_type_ const& fold,
        lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {},
        compaction_stamps_t const& stamps = compaction_stamps_t {}) noexcept {
        if (!claim_compaction_()) return compaction_outcome_t::nothing_to_compact_k;

        expected<compaction_outcome_t> const outcome = compact_once_(reading_channel, writing_channel, placement,
                                                                     devices, cancellation, fold, published, stamps,
                                                                     merge_choice_t::overfull_k);
        atomic_reference_<std::int32_t> {compacting_}.store(static_cast<std::int32_t>(compaction_state_t::idle_k),
                                                            std::memory_order_release);
        return outcome;
    }

    /**
     *  @brief Drops every version stamped above @p ceiling, merging down each level that holds one until none does.
     *
     *  A mount runs this before anything draws a stamp: a crash may leave runs holding commits the catalog it found
     *  never promised, and those land nowhere else, so dropping them all leaves only commits that landed whole.
     */
    template <utilities::fold_concept fold_type_>
    status_t truncate_above(io_channel_t& reading_channel, io_channel_t& writing_channel, std::uint64_t ceiling,
                            fold_type_ const& fold,
                            lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {}) noexcept {
        if (!claim_compaction_()) return {status_code_t::invalid_argument_k, "A mount truncates a tree nobody merges"};
        compaction_stamps_t stamps;
        stamps.horizon = ceiling;
        stamps.ceiling = ceiling;
        stamps.floor = ceiling;
        status_t result;
        while (result.succeeded() && above_(*levels_, ceiling) != levels_->size()) {
            expected<compaction_outcome_t> const outcome = compact_once_(
                reading_channel, writing_channel, compaction_placement_t::host_k, {}, nullptr, fold, published, stamps,
                merge_choice_t::above_ceiling_k);
            if (!outcome) result = outcome.status();
        }
        atomic_reference_<std::int32_t> {compacting_}.store(static_cast<std::int32_t>(compaction_state_t::idle_k),
                                                            std::memory_order_release);
        return result;
    }

#pragma endregion Publication

  private:

#pragma region Private Types

    /** Which level one merge pass takes down into the level below it. */
    enum class merge_choice_t : std::uint8_t {

        /** The shallowest level over the bytes its number admits. */
        overfull_k,

        /** The shallowest level holding a version above the ceiling of the pass's stamps. */
        above_ceiling_k,
    };

    /** One block of one value's read, kept until its completion carries the block back. */
    struct pending_read_t {

        /** Which of the caller's requests this block belongs to. */
        std::size_t request = 0;

        /** Bytes of the block before the value's own bytes. */
        std::uint32_t skipped = 0;

        /** Bytes of the value this block carries. */
        std::uint32_t carried = 0;

        /** Bytes of the value that the blocks before this one carried. */
        std::uint64_t offset = 0;
    };

#pragma endregion Private Types

#pragma region Private Methods

    /** The piece of @p window that the block at @p at of @p length carries, for @p request. */
    [[nodiscard]] static pending_read_t piece_of_(value_window_t const& window, std::size_t request, std::uint64_t at,
                                                  std::uint64_t length) noexcept {
        std::uint64_t const wanted_first = window.skipped;
        std::uint64_t const wanted_last = window.skipped + window.bytes;
        std::uint64_t const first = at > wanted_first ? at : wanted_first;
        std::uint64_t const last = at + length < wanted_last ? at + length : wanted_last;
        pending_read_t piece;
        piece.request = request;
        piece.skipped = static_cast<std::uint32_t>(first - at);
        piece.carried = static_cast<std::uint32_t>(last > first ? last - first : 0);
        piece.offset = first - wanted_first;
        return piece;
    }

    /** Takes the right to publish, which only one thread holds at a time. */
    [[nodiscard]] bool claim_compaction_() noexcept {
        std::int32_t idle = static_cast<std::int32_t>(compaction_state_t::idle_k);
        return atomic_reference_<std::int32_t> {compacting_}.compare_exchange_strong(
            idle, static_cast<std::int32_t>(compaction_state_t::compacting_k), std::memory_order_acquire,
            std::memory_order_relaxed);
    }

    /** Forgets every run of @p set without freeing one, since the set that replaced it names them. */
    static void relinquish_(level_set_t& set) noexcept {
        for (std::size_t number = 0; number != set.levels_.size(); ++number) set.levels_[number].relinquish();
    }

    /** Drops and frees every run of @p set the current set does not name, and forgets the rest. */
    void abandon_(level_set_t& set, io_channel_t& channel) noexcept {
        for (std::size_t number = 0; number != set.levels_.size(); ++number)
            for (std::size_t index = 0; index != set.level(number).size(); ++index) {
                sorted_run_t const* const run = &set.level(number).run(index);
                if (names_(*levels_, run)) continue;
                [[maybe_unused]] status_t const dropped = sorted_run_t::drop(*volume_, channel, run->content_id());
                delete run;
            }
        relinquish_(set);
    }

    /** Whether @p set names @p run. */
    [[nodiscard]] static bool names_(level_set_t const& set, sorted_run_t const* run) noexcept {
        for (std::size_t number = 0; number != set.size(); ++number)
            for (std::size_t index = 0; index != set.level(number).size(); ++index)
                if (&set.level(number).run(index) == run) return true;
        return false;
    }

    /**
     *  @brief A new set naming every run of the current one but those of the levels @p first through @p last.
     *  @param[in] first The first level whose runs are left behind, which a merge consumes.
     *  @param[in] last The last such level, below @p first when the caller consumes none.
     */
    expected<std::unique_ptr<level_set_t>> carried_(std::size_t first, std::size_t last) const noexcept {
        level_set_t const& source = *levels_;
        std::unique_ptr<level_set_t> next {new (std::nothrow) level_set_t {}};
        if (!next || next->levels_.reserve(source.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A tree could not prepare its next level set"};

        for (std::size_t number = 0; number != source.size(); ++number) {
            level_t level = level_t::of(source.level(number).number(), source.level(number).order());
            if (number < first || number > last)
                if (status_t const carried = level.carry(source.level(number)); carried.failed()) {
                    level.relinquish();
                    relinquish_(*next);
                    return carried;
                }
            [[maybe_unused]] st::status_t const placed = next->levels_.push_back(std::move(level));
        }
        return next;
    }

    /**
     *  @brief Makes @p next current, calls @p published, then frees the set it replaced and drops @p consumed.
     *
     *  The hook publishes the new set to readers and returns only once none is left in the old one, so the old set
     *  and the runs only it named are unreachable by then; its other runs belong to @p next.
     */
    status_t replace_(std::unique_ptr<level_set_t> next, lsm_tree_publication_hook_t const& published,
                      io_channel_t& channel, std::span<sorted_run_t const* const> consumed) noexcept {
        if (published.prepare != nullptr)
            if (status_t const prepared = published.prepare(published.context); prepared.failed()) {
                abandon_(*next, channel);
                return prepared;
            }
        std::unique_ptr<level_set_t> replaced = std::exchange(levels_, std::move(next));
        status_t const recorded = published.notify != nullptr ? published.notify(published.context) : status_t {};
        relinquish_(*replaced);
        if (recorded.failed()) {
            for (sorted_run_t const* run : consumed) delete run;
            return recorded;
        }
        status_t first;
        for (sorted_run_t const* run : consumed) {
            if (status_t const dropped = sorted_run_t::drop(*volume_, channel, run->content_id());
                dropped.failed() && first.succeeded())
                first = dropped;
            delete run;
        }
        return first;
    }

    /** Level 0 holds F flushed runs of one write buffer each, and every level below F times the one above. */
    [[nodiscard]] std::uint64_t level_bytes_(std::size_t number) const noexcept {
        std::uint64_t const factor = config_.level_enlarge_factor;
        std::uint64_t bytes = memory_runs_max_bytes_ * factor;
        for (std::size_t step = 0; step != number; ++step) {
            if (bytes > limit<std::uint64_t>::max() / factor) return limit<std::uint64_t>::max();
            bytes *= factor;
        }
        return bytes;
    }

    /** The shallowest level of @p levels that is over its bounds, or the level count when none is. */
    [[nodiscard]] std::size_t overfull_(level_set_t const& levels) const noexcept {
        // Every level grows by the factor, and a flushed run is one write buffer, so L0 is due at factor runs.
        if (levels.size() != 0 && levels.level(0).size() >= config_.level_enlarge_factor) return 0;
        for (std::size_t number = 0; number != levels.size(); ++number)
            if (!levels.level(number).empty() && levels.level(number).size_bytes() > level_bytes_(number))
                return number;
        return levels.size();
    }

    /** The shallowest level of @p levels holding a run with a version above @p ceiling, or the level count. */
    [[nodiscard]] static std::size_t above_(level_set_t const& levels, std::uint64_t ceiling) noexcept {
        for (std::size_t number = 0; number != levels.size(); ++number)
            for (std::size_t index = 0; index != levels.level(number).size(); ++index)
                if (levels.level(number).run(index).newest_stamp() > ceiling) return number;
        return levels.size();
    }

    /** Runs one merge of the level @p choice picks and publishes it, which the claim around it keeps to one thread. */
    template <typename fold_type_>
    expected<compaction_outcome_t> compact_once_(io_channel_t& reading_channel, io_channel_t& writing_channel,
                                                 compaction_placement_t placement, std::span<int const> devices,
                                                 cancellation_t const* cancellation, fold_type_ const& fold,
                                                 lsm_tree_publication_hook_t const& published,
                                                 compaction_stamps_t const& stamps, merge_choice_t choice) noexcept {
        level_set_t const& source = *levels_;
        std::size_t const upper = choice == merge_choice_t::overfull_k ? overfull_(source)
                                                                       : above_(source, stamps.ceiling);
        if (upper == source.size()) return compaction_outcome_t::nothing_to_compact_k;
        std::size_t const lower = upper + 1;
        bool const grows = lower == source.size();
        std::size_t const last_consumed = grows ? upper : lower;

        st::basic_vector<sorted_run_t const*> consumed;
        if (status_t const recorded = record_consumed_(source, upper, last_consumed, consumed); recorded.failed())
            return recorded;

        typename merger_t::merge_request_t request;
        request.upper = &source.level(upper);
        request.lower = grows ? nullptr : &source.level(lower);
        request.number = static_cast<std::uint32_t>(lower);
        request.depth = lsm_level_depth_t::bottom_k;
        request.value_max_size = static_cast<stored_value_size_t>(config_.lsm_level_0.sorted_run.value_max_size);
        request.run_key_slots_capacity = storage_limits_t::memory_runs_max_deltas_k;
        request.run_bytes_limit = storage_limits_t::sorted_run_file_max_size_k;
        request.placement = placement;
        request.devices = devices;
        request.cancellation = cancellation;
        request.stamps = stamps;
        if (!grows && lower + 1 != source.size()) request.depth = lsm_level_depth_t::intermediate_k;

        auto [merged, merge_status] = merger_t::merge(*volume_, reading_channel, writing_channel, request, fold);
        if (merge_status.failed()) return merge_status;

        auto [next, carried_status] = carried_(upper, last_consumed);
        if (carried_status.failed()) {
            [[maybe_unused]] status_t const abandoned = merged.drop(*volume_, writing_channel);
            return carried_status;
        }
        if (grows) {
            if (next->levels_.push_back(level_t::of(static_cast<std::uint32_t>(lower), lsm_level_order_t::sorted_k)) !=
                st::status_t::success_k) {
                [[maybe_unused]] status_t const abandoned = merged.drop(*volume_, writing_channel);
                relinquish_(*next);
                return status_t {status_code_t::out_of_memory_k, "A tree could not hold another level"};
            }
        }
        next->levels_[lower] = std::move(merged);

        if (status_t const replaced = replace_(std::move(next), published, writing_channel,
                                               std::span<sorted_run_t const* const> {consumed});
            replaced.failed())
            return replaced;
        return compaction_outcome_t::published_k;
    }

    /** Collects every run of levels @p first through @p last, which the merge has already rewritten. */
    static status_t record_consumed_(level_set_t const& source, std::size_t first, std::size_t last,
                                     st::basic_vector<sorted_run_t const*>& consumed) noexcept {
        std::size_t runs = 0;
        for (std::size_t number = first; number <= last && number != source.size(); ++number)
            runs += source.level(number).size();
        if (consumed.reserve(runs) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A tree could not list the runs it retires"};
        for (std::size_t number = first; number <= last && number != source.size(); ++number)
            for (std::size_t index = 0; index != source.level(number).size(); ++index) {
                sorted_run_t const* run = &source.level(number).run(index);
                [[maybe_unused]] st::status_t const placed = consumed.push_back(std::move(run));
            }
        return {};
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The volume every run of this tree lives on, null until the tree is mounted or once it is moved from. */
    volume_t* volume_ = nullptr;

    /** How fast the bytes a level admits grow with depth, and the value sizes of its runs. */
    lsm_tree_config_t config_ {};

    /** Bytes one flushed write buffer holds, which level zero admits F of. */
    std::uint64_t memory_runs_max_bytes_ = 0;

    /** The level set the last publication made current, which owns every run it names. */
    std::unique_ptr<level_set_t> levels_;

    /** A @c compaction_state_t, moved by @c claim_compaction_ alone. */
    std::int32_t compacting_ = 0;

#pragma endregion Private Members
};

#pragma endregion Tree

#pragma region Records

/** Appends a record for every run of @p tree to @p into, level by level and oldest first inside a level. */
template <typename tree_type_>
status_t record_lsm_tree_runs(tree_type_ const& tree, st::basic_vector<lsm_tree_run_record_t>& into) noexcept {
    typename tree_type_::level_set_t const& taken = tree.levels();
    std::size_t counted = 0;
    for (std::size_t number = 0; number != taken.size(); ++number) counted += taken.level(number).size();
    if (into.reserve(into.size() + counted) != st::status_t::success_k)
        return {status_code_t::out_of_memory_k, "No room to name the runs a tree holds"};

    for (std::size_t number = 0; number != taken.size(); ++number) {
        auto const& level = taken.level(number);
        for (std::size_t index = 0; index != level.size(); ++index) {
            lsm_tree_run_record_t record;
            record.level_number = static_cast<std::uint32_t>(number);
            record.content_id = level.run(index).content_id();
            [[maybe_unused]] st::status_t const placed = into.push_back(std::move(record));
        }
    }
    return {};
}

#pragma endregion Records

#pragma endregion LSM Tree

} // namespace unum::ustore::blobs
