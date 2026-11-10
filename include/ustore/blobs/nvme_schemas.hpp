/**
 *  @file include/ustore/blobs/nvme_schemas.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The background states of the engine's jobs, and the configurations and on-disk schemas of
 *      UStoreNVMe, kept as JSON through yyjson and validated against @c storage_limits_t.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::int32_t`, `std::int64_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcmp`

#include <chrono>      // `std::chrono::system_clock`
#include <concepts>    // `std::convertible_to`, `std::invocable`, `std::same_as`, `std::unsigned_integral`
#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::forward`, `std::move`

#include <fmt/format.h>
#include <yyjson.h>

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/blobs/nvme_key_slot.hpp"   // `storage_limits_t`, `stored_value_size_t`, `value_layout_t`
#include "ustore/utilities/nvme_mounts.hpp" // `resources_config_t`
#include "ustore/utilities/types.hpp"       // `status_t`, `uuid128_t`, `save_fields`

namespace unum::ustore::blobs {

using utilities::load;
using utilities::resources_config_t;
using utilities::save_fields;
using utilities::serializable_concept;
using utilities::storage_requirements;
using utilities::string_t;
using utilities::string_view_t;
using utilities::uuid_from_text;
using utilities::uuid_text_length_k;
using utilities::uuid_to_text;

#pragma region Background States

/** The clock every timestamp of the engine is taken from. */
using clock_t = std::chrono::system_clock;

/** One instant on that clock, as a snapshot, a transaction and a watch record it. */
using time_point_t = clock_t::time_point;

/** Readers still inside a retired slot, which a publisher waits on before freeing it. */
using readers_count_t = std::int32_t;

/** Whether the owner of a background job or a merge still wants it, moved by compare-exchange
 *  on a word. */
enum class cancellation_state_t : std::int32_t {
    wanted_k = 0,
    cancelled_k = 1,
};

/** Whether a thread holds the right to compact a tree, moved by compare-exchange on a word. */
enum class compaction_state_t : std::int32_t {
    idle_k = 0,
    compacting_k = 1,
};

#pragma endregion Background States

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
    uuid128_t collection {};
};

/** Configuration of one collection. */
struct collection_config_t {
    lsm_tree_config_t lsm_tree;
    std::size_t concurrency_limit = 0;
    std::size_t cache_limit = 0;
    std::size_t memory_runs_max_bytes = 0;
    std::size_t memory_runs_max_deltas = 0;
};

/** Delta limits of one transaction, which the @c transaction_max_* fields of the NVMe
 *  init call set. */
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
    using sorted_runs_t = st::vector<sorted_run_schema_t>;

    sorted_runs_t sorted_runs;
};

/** The levels of one LSM tree. */
struct lsm_tree_schema_t {
    using lsm_levels_t = st::vector<lsm_level_schema_t>;

    lsm_levels_t lsm_levels;
};

/** The files of one collection. */
struct collection_schema_t {
    lsm_tree_schema_t lsm_tree;
};

/** The files a saved snapshot of one collection shares with its source; saved snapshots are
 *  not yet written. */
struct collection_snapshot_schema_t {
    lsm_tree_schema_t lsm_tree;
    collection_id_t source_id;
};

/** The files and snapshots of one single-collection database. */
struct single_collection_database_schema_t {
    static constexpr schema_kind_t schema_kind_k = schema_kind_t::database_k;
    static constexpr database_kind_t database_kind_k = database_kind_t::single_collection_database_k;

    /** One saved snapshot of the single-collection database's collection, which
     *  nothing writes yet. */
    struct snapshot_t {
        collection_snapshot_schema_t collection;
        snapshot_id_t id = invalid_snapshot_id_k;
        time_point_t time_stamp = (time_point_t::max)();
    };
    using snapshots_t = st::vector<snapshot_t>;

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
        using collection_snapshots_schema_t = st::vector<collection_t>;

        snapshot_id_t id = invalid_snapshot_id_k;
        time_point_t time_stamp = (time_point_t::max)();
        collection_snapshots_schema_t collections;
    };

    using collections_t = st::vector<collection_t>;
    using snapshots_t = st::vector<snapshot_t>;

    database_id_t id;
    collections_t collections;
    snapshots_t snapshots;
};

/** A schema saved as a document of its own. */
template <typename schema_type_>
concept database_schema_concept = std::same_as<schema_type_, single_collection_database_schema_t> ||
                                  std::same_as<schema_type_, database_schema_t>;

/** A schema saved as bytes behind a signed and versioned header, as backups are. */
template <typename schema_type_>
concept binary_schema_concept = serializable_concept<schema_type_> && requires {
    { schema_type_::schema_kind_k } -> std::convertible_to<schema_kind_t>;
    { schema_type_::database_kind_k } -> std::convertible_to<database_kind_t>;
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
template <binary_schema_concept schema_type_>
std::size_t binary_schema_storage_requirements(schema_type_ const& schema) noexcept {
    return sizeof(binary_schema_header_t) + schema.storage_requirements();
}

/** Saves the header of @p schema, then the schema itself. */
template <binary_schema_concept schema_type_>
status_t save_binary_schema(schema_type_ const& schema, bytes_span_t destination) noexcept {
    binary_schema_header_t header;
    header.schema_kind = schema_type_::schema_kind_k;
    header.database_kind = schema_type_::database_kind_k;
    return save_fields(destination, header, schema);
}

/** Loads a schema behind its header, rejecting another signature, kind or version. */
template <binary_schema_concept schema_type_>
status_t load_binary_schema(schema_type_& schema, bytes_view_t source) noexcept {
    binary_schema_header_t header;
    if (status_t const status = load(header, source); status.failed()) return status;
    binary_schema_header_t const current;
    if (std::memcmp(header.signature, current.signature, sizeof(current.signature)) != 0)
        return status_t {status_code_t::malformed_k, "Invalid signature"};
    if (header.schema_kind != schema_type_::schema_kind_k || header.database_kind != schema_type_::database_kind_k)
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

/** Reads an unsigned number that fits @p value_type_. */
template <std::unsigned_integral value_type_>
[[nodiscard]] bool read_json(yyjson_val* value, value_type_& number) noexcept {
    if (!yyjson_is_uint(value) || yyjson_get_uint(value) > (std::numeric_limits<value_type_>::max)()) return false;
    number = static_cast<value_type_>(yyjson_get_uint(value));
    return true;
}

/** Reads a string. */
[[nodiscard]] inline bool read_json(yyjson_val* value, string_t& text) noexcept {
    if (!yyjson_is_str(value)) return false;
    return sz::succeeded(text.assign(string_view_t {yyjson_get_str(value), yyjson_get_len(value)}));
}

/** Reads a UUID from its printed form. */
[[nodiscard]] inline bool read_json(yyjson_val* value, uuid128_t& id) noexcept {
    std::string_view const text = json_string_view(value);
    if (text.size() != uuid128_t::printed_length_k) return false;
    std::span<char const, uuid128_t::printed_length_k> const printed {text.data(), uuid128_t::printed_length_k};
    if (!uuid128_t::parses(printed)) return false;
    id = uuid128_t::parse(printed);
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
    else if (yyjson_is_uint(value) &&
             yyjson_get_uint(value) <= std::uint64_t((std::numeric_limits<std::int64_t>::max)()))
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
template <typename element_type_, template <typename> typename allocator_>
[[nodiscard]] bool read_json(yyjson_val* array,
                             st::vector<element_type_, allocator_<element_type_>>& elements) noexcept {
    if (!yyjson_is_arr(array)) return false;
    elements.clear();
    if (elements.reserve(yyjson_arr_size(array)) != st::status_t::success_k) return false;
    yyjson_arr_iter iterator;
    yyjson_arr_iter_init(array, &iterator);
    while (yyjson_val* const item = yyjson_arr_iter_next(&iterator)) {
        element_type_ element;
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
template <database_schema_concept schema_type_>
status_t load_schema(char const* file_path, schema_type_& schema) noexcept {
    yyjson_read_err error;
    yyjson_doc* const document = yyjson_read_file(file_path, YYJSON_READ_NOFLAG, nullptr, &error);
    if (!document && error.code == YYJSON_READ_ERROR_FILE_OPEN)
        return status_t {status_code_t::not_found_k, "File not exists"};
    if (!document) return status_t {status_code_t::malformed_k, "Invalid schema"};

    yyjson_val* const root = yyjson_doc_get_root(document);
    schema_header_t header;
    status_t status = read_schema_header(root, header);
    if (status &&
        (header.schema_kind != schema_type_::schema_kind_k || header.database_kind != schema_type_::database_kind_k))
        status = status_t {status_code_t::malformed_k, "Invalid schema"};
    if (status && !read_json(yyjson_obj_get(root, "schema"), schema))
        status = status_t {status_code_t::malformed_k, "Invalid schema"};
    yyjson_doc_free(document);
    return status;
}

#pragma endregion Reading

#pragma region Writing

/** Adds @p value to @p object under the literal @p key, answering whether yyjson
 *  allocated every part. */
[[nodiscard]] inline bool add_json_member(yyjson_mut_doc* document, yyjson_mut_val* object, char const* key,
                                          yyjson_mut_val* value) noexcept {
    return object && value && yyjson_mut_obj_add(object, yyjson_mut_str(document, key), value);
}

template <std::unsigned_integral value_type_>
[[nodiscard]] yyjson_mut_val* write_json(yyjson_mut_doc* document, value_type_ number) noexcept {
    return yyjson_mut_uint(document, number);
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document, string_t const& text) noexcept {
    return yyjson_mut_str(document, text.c_str());
}

[[nodiscard]] inline yyjson_mut_val* write_json(yyjson_mut_doc* document, uuid128_t const& id) noexcept {
    char text[uuid128_t::printed_length_k] = {};
    id.print(text);
    return yyjson_mut_strncpy(document, text, uuid128_t::printed_length_k);
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

template <typename element_type_, template <typename> typename allocator_>
[[nodiscard]] yyjson_mut_val* write_json(
    yyjson_mut_doc* document, st::vector<element_type_, allocator_<element_type_>> const& elements) noexcept {
    yyjson_mut_val* const array = yyjson_mut_arr(document);
    if (!array) return nullptr;
    for (element_type_ const& element : elements)
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
template <database_schema_concept schema_type_>
status_t save_schema(char const* file_path, schema_type_ const& schema) noexcept {
    yyjson_mut_doc* const document = yyjson_mut_doc_new(nullptr);
    if (!document) return status_t {status_code_t::out_of_memory_k, "Failed to allocate memory"};

    yyjson_mut_val* const root = yyjson_mut_obj(document);
    bool const written =
        add_json_member(document, root, "version", yyjson_mut_str(document, format_version_k)) &&
        add_json_member(document, root, "schema_kind",
                        yyjson_mut_uint(document, static_cast<std::uint8_t>(schema_type_::schema_kind_k))) &&
        add_json_member(document, root, "database_kind",
                        yyjson_mut_uint(document, static_cast<std::uint8_t>(schema_type_::database_kind_k))) &&
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
template <typename sink_type_>
concept issue_sink_concept = std::invocable<sink_type_&, issue_kind_t, std::string_view>;

/** Formats an issue on the stack and hands it to @p sink, answering whether it was
 *  only a warning. */
template <issue_sink_concept sink_type_, typename... argument_types_>
bool report_issue(sink_type_& sink, issue_kind_t kind, fmt::format_string<argument_types_...> format,
                  argument_types_&&... arguments) noexcept {
    char message[256];
    auto const formatted = fmt::format_to_n(message, sizeof(message), format,
                                            std::forward<argument_types_>(arguments)...);
    sink(kind, std::string_view(message, formatted.out - message));
    return kind == issue_kind_t::warning_k;
}

/** Checks resources on their own, answering whether no error was reported. */
template <issue_sink_concept sink_type_>
[[nodiscard]] bool validate(resources_config_t const& config, sink_type_&& sink) noexcept {
    if (config.memory_limit == 0) return report_issue(sink, issue_kind_t::error_k, "Memory limit is not set");
    if (config.gpu_devices.empty() && config.gpu_memory_limit > 0)
        return report_issue(sink, issue_kind_t::error_k, "GPU memory limit is set but no GPU device is selected");
    if (!config.gpu_devices.empty() && config.gpu_memory_limit < 4 * 1024 * 1024)
        return report_issue(sink, issue_kind_t::error_k, "GPU memory limit leaves no room for device buffers");
    if (config.manifest.empty()) return report_issue(sink, issue_kind_t::error_k, "Manifest is not set");
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
template <issue_sink_concept sink_type_>
[[nodiscard]] bool validate(resources_config_t const& config, std::size_t logical_cores_count,
                            sink_type_&& sink) noexcept {
    if (!validate(config, sink)) return false;
    if (config.threads > logical_cores_count)
        return report_issue(sink, issue_kind_t::error_k, "{} threads exceed the {} logical cores", config.threads,
                            logical_cores_count);
    if (config.threads == logical_cores_count)
        report_issue(sink, issue_kind_t::warning_k, "Threads take every logical core");
    return true;
}

/** Checks the delta limits of one transaction against the storage limits. */
template <issue_sink_concept sink_type_>
[[nodiscard]] bool validate(transaction_config_t const& config, sink_type_&& sink) noexcept {
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

/** Checks the limits of one collection holding values of @p value_layout against
 *  the storage limits. */
template <issue_sink_concept sink_type_>
[[nodiscard]] bool validate(user_collection_config_t const& config, value_layout_t value_layout,
                            sink_type_&& sink) noexcept {
    if ((config.value_size == 0) == (config.value_max_size == 0))
        return report_issue(sink, issue_kind_t::error_k, "Exactly one of value_size and value_max_size must be set");

    if (is_inline(value_layout)) {
        auto const largest = static_cast<stored_value_size_t>(value_layout_t::inline_44_bytes_k);
        if (config.value_size < 1 || config.value_size > largest)
            return report_issue(sink, issue_kind_t::error_k, "Inline value size must be within [1, {}]", largest);
        if (config.value_size > static_cast<stored_value_size_t>(value_layout))
            return report_issue(sink, issue_kind_t::error_k, "Value layout is too small for the value size");
        if (resolve_value_layout(config.value_size) != value_layout)
            report_issue(sink, issue_kind_t::warning_k, "A smaller value layout fits the value size");
        if (config.cache_limit > 0)
            report_issue(sink, issue_kind_t::warning_k, "Value cache does not serve inline values");
    }
    else if (value_layout == value_layout_t::fixed_width_k) {
        if (config.value_size < storage_limits_t::fixed_value_min_size_k ||
            config.value_size > storage_limits_t::fixed_value_max_size_k)
            return report_issue(sink, issue_kind_t::error_k, "Fixed value size must be within [{}, {}]",
                                storage_limits_t::fixed_value_min_size_k, storage_limits_t::fixed_value_max_size_k);
        if (config.value_size <= static_cast<stored_value_size_t>(value_layout_t::inline_44_bytes_k))
            report_issue(sink, issue_kind_t::warning_k, "An inline value layout fits the value size");
    }
    else if (config.value_max_size < storage_limits_t::variable_value_min_size_k ||
             config.value_max_size > storage_limits_t::variable_value_max_size_k)
        return report_issue(sink, issue_kind_t::error_k, "Variable value size must be within [{}, {}]",
                            storage_limits_t::variable_value_min_size_k, storage_limits_t::variable_value_max_size_k);

    std::size_t const value_max_size = fu::max_of(config.value_size, config.value_max_size);
    if (!is_inline(value_layout)) {
        if (config.cache_limit > storage_limits_t::cache_limit_max_size_k)
            return report_issue(sink, issue_kind_t::error_k, "Value cache must not exceed {} bytes",
                                storage_limits_t::cache_limit_max_size_k);
        if (config.cache_limit / value_max_size > (std::numeric_limits<std::uint32_t>::max)())
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
template <issue_sink_concept sink_type_>
[[nodiscard]] bool validate(resources_config_t const& resources, database_config_t const& config,
                            sink_type_&& sink) noexcept {
    if (!validate(resources, sink) || !validate(config.default_transaction, sink)) return false;
    if (resources.memory_limit < config.default_transaction.max_bytes)
        return report_issue(sink, issue_kind_t::error_k, "Memory limit cannot hold one transaction");
    return true;
}

/** Checks resources and the limits of a single-collection database within them. */
template <issue_sink_concept sink_type_>
[[nodiscard]] bool validate(resources_config_t const& resources, user_single_collection_database_config_t const& config,
                            sink_type_&& sink) noexcept {
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
template <issue_sink_concept sink_type_>
[[nodiscard]] bool validate(resources_config_t const& resources, user_collection_config_t const& config,
                            sink_type_&& sink) noexcept {
    if (!validate(resources, sink) || !validate(config, resolve_value_layout(config.value_size), sink)) return false;
    if (resources.memory_limit < config.memory_runs_max_bytes)
        return report_issue(sink, issue_kind_t::error_k, "Memory limit cannot hold the memory runs");
    return true;
}

#pragma endregion Validation

#pragma endregion Configurations and Schemas

} // namespace unum::ustore::blobs
