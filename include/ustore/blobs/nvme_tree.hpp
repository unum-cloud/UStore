/**
 *  @file include/ustore/blobs/nvme_tree.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The configurations and schemas of UStoreNVMe, the levels of one LSM tree, the merge of one level into
 *      the next, and the level set readers see.
 *
 *  Configurations and on-disk schemas are kept as JSON through yyjson and validated against @c storage_limits_t.
 *
 *  Level zero takes one run per flush of the memory runs, so its runs overlap and only their age
 *  tells which version of a key is current: a lookup walks them newest first and a scan merges them
 *  by smallest key. Every deeper level is written by a merge, so its runs are disjoint and
 *  ascending and a lookup is a binary search over their bounds. Both shapes carry remove deltas;
 *  only the bottom level may forget one, which is a property of the merge that writes it rather
 *  than of the level that holds it.
 *
 *  A level names its runs by handles into the database's run pool, whose records never move, so the
 *  level set a compaction publishes can name the same runs the set it replaces named. Ownership
 *  follows one rule and no other:
 *  a run counts the level sets naming it, at most the current one and the next, and is given back
 *  to its volume when the last of them lets it go.
 *
 *  The merge itself is arithmetic over key slots and belongs to @c nvme_compaction.hpp, which
 *  decides who survives, cuts the survivors into runs that fit their bounds and plans where every
 *  value moves. What is left to the merger is the I/O: pointing the compactor at each source run's
 *  index, then writing the planned runs out of the plan it answered. Reads and writes ride separate
 *  channels because a channel is one queue pair with one completion queue, so a read pipeline
 *  polling on the channel a write pipeline submits on would swallow the writes' completions.
 *
 *  A flush writes one run, and a compaction merges a whole level zero or one deeper run with the
 *  runs below that it overlaps, or moves a run that overlaps nothing down untouched. Both publish
 *  by building the next level set in the spare beside the current one, which no one changes once a
 *  reader may hold it. The tree hands the new set to the publication hook its caller names, which
 *  publishes it to readers and returns only once no reader is left in the set it replaced; that set
 *  is emptied then, which is the whole of the reclamation, and readers never wait on a writer. A
 *  batched read refills the ring as completions retire rather than submitting a batch and waiting
 *  on it, so the depth a read reaches is the queue pair's and not the batch's.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::int32_t`, `std::int64_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcmp`, `std::memcpy`, `std::memmove`, `std::memset`

#include <algorithm>   // `std::max`, `std::min`
#include <chrono>      // `std::chrono::system_clock`
#include <concepts>    // `std::invocable`, `std::same_as`, `std::unsigned_integral`
#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::exchange`, `std::forward`, `std::move`

#include <fmt/format.h>
#include <yyjson.h>

#include <forkunion/atomics.hpp>             // `std::memory_order_acquire`, `std::memory_order_release`
#include <smashtable/basic_growing_pool.hpp> // `st::basic_growing_pool`
#include <smashtable/basic_vector.hpp>       // `st::vector`

#include "ustore/blobs/nvme_compaction.hpp"        // `host_compactor_t`, `compaction_output`, `compaction_kind_t`
#include "ustore/blobs/nvme_key_slot.hpp"          // `storage_limits_t`, `stored_value_size_t`, `value_layout_t`
#include "ustore/blobs/nvme_sorted_run.hpp"        // `lsm_sorted_run`, `sorted_run_header_t`, `sorted_run_layout`
#include "ustore/blobs/vram_nvidia_compaction.hpp" // `cuda_compactor_t`
#include "ustore/utilities/nvme_channels.hpp"      // `io_channel_t`, `basic_read_pipeline`
#include "ustore/utilities/nvme_mounts.hpp"        // `resources_config_t`
#include "ustore/utilities/nvme_ring.hpp"          // `extent_t`, `ring_concept`, `portable_alignment_k`
#include "ustore/utilities/nvme_slots.hpp"         // `slot_bytes_k`, `slot_payload_offset_k`
#include "ustore/utilities/nvme_volume.hpp"        // `medium_extent_t`
#include "ustore/utilities/ram_buffers.hpp"        // `constrained_allocator`, `resource_constraint`
#include "ustore/utilities/types.hpp" // `status_t`, `expected`, `byte_t`, `uuid_t`, `uuid_generator`, `save_fields`, `load_fields`

namespace unum::ustore::blobs {

using utilities::constrained_allocator;
using utilities::fields_storage_requirements;
using utilities::load;
using utilities::load_fields;
using utilities::medium_reference_t;
using utilities::pooled_buffer_t;
using utilities::resource_constraint;
using utilities::resources_config_t;
using utilities::ring_concept;
using utilities::save;
using utilities::save_fields;
using utilities::serializable_concept;
using utilities::storage_requirements;
using utilities::string_t;
using utilities::string_view_t;
using utilities::uuid_from_text;
using utilities::uuid_generator;
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
    uuid_t collection {};
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
        time_point_t time_stamp = time_point_t::max();
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
    if (!yyjson_is_uint(value) || yyjson_get_uint(value) > std::numeric_limits<value_type_>::max()) return false;
    number = static_cast<value_type_>(yyjson_get_uint(value));
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

/** Adds @p value to @p object under the literal @p key, answering whether yyjson allocated every part. */
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

/** Formats an issue on the stack and hands it to @p sink, answering whether it was only a warning. */
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

/** Checks the limits of one collection holding values of @p value_layout against the storage limits. */
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

#pragma region Record Pools

/**
 *  @brief Records of one kind that never move, taken and vacated by the threads of one
 *      store or database.
 *
 *  A holder names a record by a handle, its index and the generation it was taken at. A record
 *  lives from @c take until @c vacate, which steps the generation, so a handle kept past that
 *  resolves to nothing. Takes and vacates from several threads go through one claim word; resolving
 *  a handle reads only its record, which its holder keeps alive. Every byte of the records is
 *  charged to the budget the pool was made with.
 *
 *  @tparam record_type_ What a record holds, whose move assignment carries its payload and
 *      keeps its generation and any list links, so a taken record is filled and a vacated one
 *      emptied in place.
 *  @tparam index_type_ Names a record, and is the width of its generation.
 *  @tparam atomic_reference_ The reference the claim word and the budget's word are owned through.
 *  @tparam waiter_type_ What the core does while another thread holds the claim.
 */
template <typename record_type_, std::unsigned_integral index_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
class lsm_record_pool {
  public:

#pragma region Types

    /** One record of the pool. */
    using record_t = record_type_;

    /** Names a record. */
    using index_t = index_type_;

    /** The budget the records are charged to. */
    using constraint_t = resource_constraint<atomic_reference_>;

    /** The records and the stack of the free ones, in chunks charged to the budget, which a reader
     *  resolves without the claim while a take grows them. */
    using records_t = st::basic_growing_pool<record_t, index_t, constrained_allocator<std::byte, atomic_reference_>>;

    /** A record's index and the generation it was taken at. */
    using handle_t = typename records_t::handle_t;

    /** What @c take answers when every record is taken. */
    static constexpr index_t absent_k = records_t::absent_k;

#pragma endregion Types

#pragma region Construction

    lsm_record_pool() noexcept = default;
    lsm_record_pool(lsm_record_pool const&) = delete;
    lsm_record_pool& operator=(lsm_record_pool const&) = delete;
    lsm_record_pool(lsm_record_pool&&) noexcept = default;
    lsm_record_pool& operator=(lsm_record_pool&&) noexcept = default;

    /** A pool of @p capacity free records charged to @p constraint, which must outlive it. */
    static expected<lsm_record_pool> make(std::size_t capacity, constraint_t& constraint) noexcept {
        using allocator_t = constrained_allocator<std::byte, atomic_reference_>;
        return adopt_(records_t::make(capacity, capacity, allocator_t {constraint}));
    }

    /** A pool of @p capacity free records that grows by chunks up to @p capacity_limit when a
     *  @c take finds none, charged to @p constraint, which must outlive it. */
    static expected<lsm_record_pool> make(std::size_t capacity, std::size_t capacity_limit,
                                          constraint_t& constraint) noexcept {
        using allocator_t = constrained_allocator<std::byte, atomic_reference_>;
        return adopt_(records_t::make(capacity, capacity_limit, allocator_t {constraint}));
    }

#pragma endregion Construction

#pragma region Records

    /** Moves @p record into a free record and answers its handle, leaving @p record untouched when
     *  every record is taken. */
    expected<handle_t> take(record_t&& record) noexcept {
        lock_();
        index_t const index = records_.take();
        unlock_();
        if (index == absent_k) return status_t {status_code_t::capacity_exhausted_k, "Every record of a pool is taken"};
        records_[index] = std::move(record);
        return records_.handle_of(index);
    }

    /** Records taken. */
    [[nodiscard]] std::size_t size() const noexcept { return records_.size(); }

    /** Records the pool holds, taken or free. */
    [[nodiscard]] std::size_t capacity() const noexcept { return records_.capacity(); }

    /** The record @p record names, or null once it was vacated. */
    [[nodiscard]] record_t* at(handle_t record) noexcept { return records_.at(record); }

    /** The record @p record names, or null once it was vacated. */
    [[nodiscard]] record_t const* at(handle_t record) const noexcept { return records_.at(record); }

    /** The record at @p index, taken or not, which list links reach by index. */
    [[nodiscard]] record_t& operator[](index_t index) noexcept { return records_[index]; }

    /** The record at @p index, taken or not. */
    [[nodiscard]] record_t const& operator[](index_t index) const noexcept { return records_[index]; }

    /** The handle naming @p record, which is a record of this pool. */
    [[nodiscard]] handle_t handle_of(record_t const& record) const noexcept {
        return records_.handle_of(records_.index_of(record));
    }

    /** Empties the record @p record names and frees it, so every copy of the handle
     *  resolves to nothing. */
    void vacate(handle_t record) noexcept {
        record_t* const held = records_.at(record);
        if (held == nullptr) return;
        *held = record_t {};
        lock_();
        records_.vacate(record.index);
        unlock_();
    }

    /** Gives back the extent of every run @p runs names to @p volume and vacates them, for runs
     *  no manifest names. */
    template <typename volume_type_>
    void discard(volume_type_& volume, std::span<handle_t const> runs) noexcept {
        for (handle_t const run : runs)
            if (record_t const* const held = records_.at(run); held != nullptr) {
                volume.deallocate(held->extent());
                vacate(run);
            }
    }

#pragma endregion Records

  private:

#pragma region Private Methods

    /** A pool over the records @p made, or the status SmashTable refused them with. */
    static expected<lsm_record_pool> adopt_(st::expected<records_t>&& made) noexcept {
        if (made.status() == st::status_t::invalid_argument_k)
            return status_t {status_code_t::invalid_argument_k, "A pool holds fewer records than its index names"};
        if (!made) return status_t {status_code_t::out_of_memory_k, "No room for the records of a pool"};
        lsm_record_pool pool;
        pool.records_ = std::move(*made);
        return pool;
    }

    /** Takes the claim word, which guards the free-index stack alone. */
    void lock_() noexcept {
        atomic_reference_<std::int32_t> const word {claim_};
        std::int32_t expected_free = 0;
        while (!word.compare_exchange_weak(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
            waiter_type_ {}(&claim_, expected_free, std::size_t {0});
            expected_free = 0;
        }
    }

    /** Lets the claim word go. */
    void unlock_() noexcept { atomic_reference_<std::int32_t> {claim_}.store(0, std::memory_order_release); }

#pragma endregion Private Methods

#pragma region Private Members

    /** The records and the stack of the free ones. */
    records_t records_;

    /** Zero while no thread takes or vacates a record. */
    std::int32_t claim_ = 0;

#pragma endregion Private Members
};

/** Every run the trees of one database and one key slot hold, at most 65,534, which is what a
 *  request token packs. */
template <typename sorted_run_type_, template <typename> typename atomic_reference_, typename waiter_type_>
using lsm_run_pool = lsm_record_pool<sorted_run_type_, std::uint16_t, atomic_reference_, waiter_type_>;

#pragma endregion Record Pools

#pragma region Level

/**
 *  @brief The sorted runs of one level, answering lookups and scans over all of them at once.
 *
 *  A level names its runs by handles into the database's run pool, so a successor level set names
 *  the same records rather than copying the index behind them and a reader of either set reads the
 *  same bytes. The level owns none of them; its tree decides when nothing names a run.
 *
 *  @tparam atomic_reference_ The reference the run pool's words are owned through.
 *  @tparam waiter_type_ What the core does while a run waits for its reads.
 */
template <typename key_slot_type_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiter_type_>
class lsm_level {
  public:

#pragma region Types

    /** The key slot the runs of this level hold. */
    using key_slot_t = key_slot_type_;

    /** The volume the runs of this level live on. */
    using volume_t = volume_type_;

    /** One immutable run of this level. */
    using sorted_run_t = lsm_sorted_run<key_slot_t, volume_t, waiter_type_>;

    /** The pool the runs of this level live in. */
    using run_pool_t = lsm_run_pool<sorted_run_t, atomic_reference_, waiter_type_>;

    /** How this level names one of its runs. */
    using run_handle_t = typename run_pool_t::handle_t;

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
        : runs_(std::move(other.runs_)), run_pool_(other.run_pool_), number_(other.number_), order_(other.order_),
          size_bytes_(std::exchange(other.size_bytes_, 0)),
          population_count_(std::exchange(other.population_count_, 0)),
          lower_(std::exchange(other.lower_, limit<stored_key_t>::max())),
          upper_(std::exchange(other.upper_, limit<stored_key_t>::min())) {}

    lsm_level& operator=(lsm_level&& other) noexcept {
        if (this == &other) return *this;
        runs_ = std::move(other.runs_);
        run_pool_ = other.run_pool_;
        number_ = other.number_;
        order_ = other.order_;
        size_bytes_ = std::exchange(other.size_bytes_, 0);
        population_count_ = std::exchange(other.population_count_, 0);
        lower_ = std::exchange(other.lower_, limit<stored_key_t>::max());
        upper_ = std::exchange(other.upper_, limit<stored_key_t>::min());
        return *this;
    }

    /** An empty level numbered @p number of runs from @p runs, read in the order @p order names. */
    [[nodiscard]] static lsm_level of(run_pool_t& runs, std::uint32_t number, lsm_level_order_t order) noexcept {
        lsm_level level;
        level.run_pool_ = &runs;
        level.number_ = number;
        level.order_ = order;
        return level;
    }

    /** Names @p run, at the back for an overlapping level and in key order for a sorted one, which
     *  it must not overlap. */
    status_t insert_run(run_handle_t run) noexcept {
        if (runs_.reserve(runs_.size() + 1) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A level could not hold another run"};
        sorted_run_t const& inserted = *run_pool_->at(run);
        std::size_t position = runs_.size();
        if (order_ == lsm_level_order_t::sorted_k) {
            std::size_t low = 0;
            while (low != position) {
                std::size_t const middle = low + (position - low) / 2;
                if (run_at_(middle).precedes(inserted)) low = middle + 1;
                else position = middle;
            }
            if (position != runs_.size() && !inserted.precedes(run_at_(position)))
                return {status_code_t::invalid_argument_k, "A sorted level takes runs that overlap no other"};
        }
        note_(inserted);
        [[maybe_unused]] st::status_t const placed = runs_.insert(position, std::move(run));
        return {};
    }

    /** Names every run @p other names but those @p skipped lists, in the order @p other holds
     *  them, which is how a prepared level set carries a level a merge leaves alone or
     *  consumes in part. */
    status_t carry(lsm_level const& other, std::span<sorted_run_t const* const> skipped) noexcept {
        if (runs_.reserve(runs_.size() + other.runs_.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A level could not name that many runs"};
        for (std::size_t index = 0; index != other.runs_.size(); ++index) {
            sorted_run_t const* const named = &other.run_at_(index);
            bool consumed = false;
            for (sorted_run_t const* const picked : skipped) consumed |= picked == named;
            if (consumed) continue;
            note_(*named);
            [[maybe_unused]] st::status_t const placed = runs_.push_back(run_handle_t {other.runs_[index]});
        }
        return {};
    }

    /** Makes room for @p count more runs, so a publication that places them all fails before
     *  it names any. */
    status_t reserve(std::size_t count) noexcept {
        if (runs_.reserve(runs_.size() + count) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A level could not hold that many runs"};
        return {};
    }

    /** Forgets every run, which the tree does once it stops counting this level among those
     *  naming them. */
    void clear() noexcept {
        runs_.clear();
        size_bytes_ = 0;
        population_count_ = 0;
        lower_ = limit<stored_key_t>::max();
        upper_ = limit<stored_key_t>::min();
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
    [[nodiscard]] sorted_run_t const& run(std::size_t index) const noexcept { return run_at_(index); }

    /** The handles of the runs, in the order @c run answers them. */
    [[nodiscard]] std::span<run_handle_t const> runs() const noexcept { return std::span<run_handle_t const> {runs_}; }

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
                sorted_run_t const& run = run_at_(index);
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
            sorted_run_t const& run = run_at_(middle);
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
    [[nodiscard]] key_slot_t key_slot(lsm_level_position_t position) const noexcept {
        return run_at_(position.run_index).key_slot(position.key_slot_position);
    }

    /** Where the value of the key slot at @p position sits, empty for one that needs no read. */
    [[nodiscard]] value_window_t value_window(lsm_level_position_t position) const noexcept {
        return run_at_(position.run_index).value_window(position.key_slot_position);
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
                run_at_(index).for_each_key_slot([&](std::uint64_t at, key_slot_t const& slot) noexcept {
                    visitor(lsm_level_position_t {static_cast<std::uint32_t>(index), at}, slot);
                });
            return {};
        }

        st::vector<std::uint64_t> cursors;
        if (cursors.resize(runs_.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A level could not track a cursor per run"};
        for (std::size_t index = 0; index != runs_.size(); ++index) cursors[index] = 0;

        for (;;) {
            std::size_t chosen = runs_.size();
            stored_key_t smallest = limit<stored_key_t>::max();
            for (std::size_t index = 0; index != runs_.size(); ++index) {
                if (cursors[index] == run_at_(index).population_count()) continue;
                stored_key_t const key = run_at_(index).key_slot(cursors[index]).stored_key;
                if (chosen != runs_.size() && smallest < key) continue;
                chosen = index;
                smallest = key;
            }
            if (chosen == runs_.size()) return {};

            visitor(lsm_level_position_t {static_cast<std::uint32_t>(chosen), cursors[chosen]},
                    run_at_(chosen).key_slot(cursors[chosen]));
            for (std::size_t index = 0; index != runs_.size(); ++index)
                if (cursors[index] != run_at_(index).population_count() &&
                    run_at_(index).key_slot(cursors[index]).stored_key == smallest)
                    ++cursors[index];
        }
    }

#pragma endregion Lookups

  private:

#pragma region Private Methods

    /** The run at @p index, whose record the level set naming it keeps alive. */
    [[nodiscard]] sorted_run_t const& run_at_(std::size_t index) const noexcept { return *run_pool_->at(runs_[index]); }

    /** Folds @p run into the bounds, the byte count and the population the level reports. */
    void note_(sorted_run_t const& run) noexcept {
        size_bytes_ += run.size_bytes();
        population_count_ += run.population_count();
        if (run.lower_stored_key() < lower_) lower_ = run.lower_stored_key();
        if (upper_ < run.upper_stored_key()) upper_ = run.upper_stored_key();
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The runs of the level, oldest first when they overlap and ascending when they do not. */
    st::vector<run_handle_t> runs_;

    /** The pool every run of the level lives in, which outlives the level. */
    run_pool_t* run_pool_ = nullptr;

    /** Where this level sits in its tree. */
    std::uint32_t number_ = 0;

    /** Whether the runs may overlap. */
    lsm_level_order_t order_ = lsm_level_order_t::sorted_k;

    /** Bytes every run takes, kept as runs are named rather than recounted. */
    std::uint64_t size_bytes_ = 0;

    /** Key slots every run holds, kept as runs are named rather than recounted. */
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

    /** @c cuda_compactor_t, on the device of the executor the database owns. */
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
 *  @brief Merges runs of one level with the runs of the level below that they overlap, writing
 *      new sorted runs.
 *
 *  The tree picks the runs, which are a whole level zero or one run of a deeper level, and the runs
 *  below that overlap them; the new runs replace exactly those, so every source run is free once
 *  the merge is published and no run of either level outside the picked range is read or written.
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
    using level_t = lsm_level<key_slot_t, volume_t, atomic_reference_, waiter_type_>;

    /** One immutable run of a level. */
    using sorted_run_t = typename level_t::sorted_run_t;

    /** The pool the new runs are taken into. */
    using run_pool_t = typename level_t::run_pool_t;

    /** How a level names one run. */
    using run_handle_t = typename level_t::run_handle_t;

    /** The runs one merge wrote, ascending and disjoint. */
    using merged_runs_t = st::vector<run_handle_t>;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The transport the volume moves bytes through. */
    using disk_t = std::remove_reference_t<decltype(std::declval<volume_type_&>().disk())>;

    /** The word a caller raises to stop a merge. */
    using cancellation_t = compaction_cancellation<atomic_reference_>;

    /** Where the sections of a run sit inside its extent. */
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

        /** The runs whose versions shadow the lower ones', oldest first when they overlap and
         *  ascending otherwise, which are never none. */
        std::span<sorted_run_t const* const> upper;

        /** Whether the upper runs may overlap, which they do on level zero alone. */
        lsm_level_order_t upper_order = lsm_level_order_t::overlapping_k;

        /** The runs of the level below that the upper ones overlap, ascending, or none. */
        std::span<sorted_run_t const* const> lower;

        /** Whether anything below the new runs overlaps their range, which decides whether
         *  remove deltas drop. */
        lsm_level_depth_t depth = lsm_level_depth_t::intermediate_k;

        /** The collection every new run's header names. */
        uuid_t collection {};

        /** The largest value the collection admits, which every new run's header carries. */
        stored_value_size_t value_max_size = 0;

        /** Largest new run in bytes, which a run of one key slot may still exceed. */
        std::uint64_t run_bytes_limit = storage_limits_t::sorted_run_file_max_size_k;

        /** Where the compaction runs. */
        compaction_placement_t placement = compaction_placement_t::automatic_k;

        /** The compactor the database picked at open: the calling thread, or its warm device compactor. */
        compactor_ref_t compactor;

        /** The word the merge reads between blocks, or null for a merge nobody stops. */
        cancellation_t const* cancellation = nullptr;

        /** Which older versions survive for readers and for a mount, and the floor the new runs count from. */
        compaction_stamps_t stamps;
    };

#pragma endregion Types

#pragma region Merging

    /**
     *  @brief Merges @p request.upper into @p request.lower and writes the survivors as
     *      new sorted runs.
     *  @param[in,out] volume The volume the new runs are allocated on, which every source run
     *      also lives on.
     *  @param[in,out] runs The pool the new runs are taken into, naming them in no level yet.
     *  @param[in,out] reading_channel The channel the source values are gathered on, held by the calling thread.
     *  @param[in,out] writing_channel The channel the new runs are written on, which must not be @p reading_channel.
     *  @param[in] request Which levels merge, what the new runs may grow to, and where the compaction runs.
     *  @param[in] fold How the collection's changes compose and apply, which reads their values
     *      on @p reading_channel before any is gathered.
     *  @param[in] device_fold How a device composes the collection's changes, or @c no_device_fold_t where none does.
     *  @return The new runs in ascending order, or the status that stopped the merge with every new
     *      run already given back.
     */
    template <utilities::fold_concept fold_type_, typename device_fold_type_>
    static expected<merged_runs_t> merge(volume_t& volume, run_pool_t& runs, io_channel_t& reading_channel,
                                         io_channel_t& writing_channel, merge_request_t const& request,
                                         fold_type_ const& fold, device_fold_type_ const& device_fold) noexcept {
        if (request.upper.empty())
            return status_t {status_code_t::invalid_argument_k, "A merge reads at least one upper run"};
        if (&reading_channel == &writing_channel)
            return status_t {status_code_t::invalid_argument_k, "A merge reads and writes on two channels"};
        if constexpr (sorted_run_t::tailed_k) return merge_strings_(volume, runs, writing_channel, request);
        else return merge_fixed_(volume, runs, reading_channel, writing_channel, request, fold, device_fold);
    }

#pragma endregion Merging

  private:

    /** Merges runs of fixed-width keys through the compactor the request names, then gathers and
     *  writes their survivors as new runs. */
    template <utilities::fold_concept fold_type_, typename device_fold_type_>
    static expected<merged_runs_t> merge_fixed_(volume_t& volume, run_pool_t& runs, io_channel_t& reading_channel,
                                                io_channel_t& writing_channel, merge_request_t const& request,
                                                fold_type_ const& fold, device_fold_type_ const& device_fold) noexcept {
        st::vector<sorted_run_t const*> sources;
        st::vector<sorted_run_index_view<key_slot_t>> views;
        if (status_t const pointed = point_views_(request.upper, request.lower, sources, views); pointed.failed())
            return pointed;

        // A fold reads the values of the changes it folds, from their runs, one at a time.
        auto read_value = [&](located_key_slot<key_slot_t> const& record, utilities::arena_allocator<byte_t>& arena,
                              std::span<byte_t const>& bytes) noexcept {
            std::size_t const size = sources[record.sorted_run]->value_window(record.position).bytes;
            byte_t* const into = size != 0 ? arena.allocate(size) : nullptr;
            if (size != 0 && into == nullptr) return st::status_t::out_of_memory_heap_k;
            if (sources[record.sorted_run]->read_value(reading_channel, record.position, {into, size}).failed())
                return st::status_t::input_output_k;
            bytes = {into, size};
            return st::status_t::success_k;
        };

        std::size_t const upper_runs = request.upper.size();
        std::span<sorted_run_index_view<key_slot_t> const> const level_views {views};
        compaction_output_t output;
        if (status_t const compacted = run_compaction_(
                kind_of_(request.upper_order, request.depth), level_views.first(upper_runs),
                level_views.subspan(upper_runs), std::span<sorted_run_t const* const> {sources}, bounds_of_(request),
                request.stamps, request.placement, request.compactor, fold, device_fold, read_value, output);
            compacted.failed())
            return compacted;

        merged_runs_t merged;
        if (merged.reserve(output.sorted_runs.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A merge could not track the runs it writes"};

        std::uint64_t first_key_slot = 0;
        std::uint64_t first_copy_range = 0;
        for (std::size_t index = 0; index != output.sorted_runs.size(); ++index) {
            planned_run_t planned;
            planned.key_slots_count = output.sorted_runs[index].key_slots_count;
            planned.stored_values_bytes = output.sorted_runs[index].stored_values_bytes;
            planned.first_key_slot = first_key_slot;
            planned.first_copy_range = first_copy_range;
            planned.copy_ranges_count = output.sorted_runs[index].value_copy_ranges_count;

            auto [written, write_run_status] = write_run_(
                volume, reading_channel, writing_channel, std::span<sorted_run_t const* const> {sources}, output,
                planned, request.collection, request.value_max_size, request.stamps.floor, request.cancellation);
            if (write_run_status.failed()) {
                runs.discard(volume, std::span<run_handle_t const> {merged});
                return write_run_status;
            }
            auto [taken, take_status] = runs.take(std::move(written));
            if (take_status.failed()) {
                volume.deallocate(written.extent());
                runs.discard(volume, std::span<run_handle_t const> {merged});
                return take_status;
            }
            [[maybe_unused]] st::status_t const placed = merged.push_back(std::move(taken));
            first_key_slot += planned.key_slots_count;
            first_copy_range += planned.copy_ranges_count;
        }
        return merged;
    }

#pragma region Strings Merge

    /** Versions in ascending order of their keys, the versions of one key newest first, and where
     *  each key's start. */
    struct string_side_t {

        /** Every version. */
        st::vector<strings_version_t> versions;

        /** Where the versions of each key start in @c versions. */
        st::vector<std::uint32_t> keys;
    };

    /** The whole key of @p version. */
    [[nodiscard]] static std::string_view key_of_(std::span<sorted_run_t const* const> sources,
                                                  strings_version_t version) noexcept {
        return sources[version.source]->key(version.position);
    }

    /** Appends every version of the runs of @p sources from @p first to @p last, which are disjoint
     *  and ascend, to @p side, noting where each key starts. */
    static status_t side_of_(std::span<sorted_run_t const* const> sources, std::uint32_t first, std::uint32_t last,
                             string_side_t& side) noexcept {
        std::uint64_t versions = 0;
        for (std::uint32_t source = first; source != last; ++source) versions += sources[source]->population_count();
        if (versions > limit<std::uint32_t>::max())
            return {status_code_t::capacity_exhausted_k, "A merge of strings holds more versions than it counts"};
        if (side.versions.reserve(static_cast<std::size_t>(versions)) != st::status_t::success_k ||
            side.keys.reserve(static_cast<std::size_t>(versions)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A merge of strings could not list its versions"};
        for (std::uint32_t source = first; source != last; ++source)
            for (std::uint64_t position = 0; position != sources[source]->population_count(); ++position) {
                strings_version_t const version {source, static_cast<std::uint32_t>(position)};
                if (position == 0 || sources[source]->key(position) != sources[source]->key(position - 1))
                    [[maybe_unused]]
                    st::status_t const noted = side.keys.push_back(st::assume_reserved,
                                                                   static_cast<std::uint32_t>(side.versions.size()));
                [[maybe_unused]] st::status_t const placed = side.versions.push_back(st::assume_reserved,
                                                                                     strings_version_t {version});
            }
        return {};
    }

    /** Folds @p older into @p newer by the union of their keys, so every key's versions from
     *  @p newer come before those from @p older, which @p newer takes in place. */
    static status_t fold_sides_(std::span<sorted_run_t const* const> sources, string_side_t& newer,
                                string_side_t const& older) noexcept {
        auto const reader_of = [sources](string_side_t const& side) noexcept {
            return [sources, &side](std::size_t first, std::span<std::string_view> views) noexcept {
                for (std::size_t index = 0; index != views.size(); ++index)
                    views[index] = key_of_(sources, side.versions[side.keys[first + index]]);
            };
        };

        std::size_t const rows_max = newer.keys.size() + older.keys.size();
        st::vector<std::size_t> newer_positions;
        st::vector<std::size_t> older_positions;
        string_side_t joined;
        if (newer_positions.resize(rows_max) != st::status_t::success_k ||
            older_positions.resize(rows_max) != st::status_t::success_k ||
            joined.versions.reserve(newer.versions.size() + older.versions.size()) != st::status_t::success_k ||
            joined.keys.reserve(rows_max) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A merge of strings could not hold its union"};
        std::size_t const rows = union_sorted_keys(reader_of(newer), newer.keys.size(), reader_of(older),
                                                   older.keys.size(), std::span<std::size_t> {newer_positions},
                                                   std::span<std::size_t> {older_positions});

        auto append_key = [&](string_side_t const& side, std::size_t key) noexcept {
            std::size_t const begin = side.keys[key];
            std::size_t const end = key + 1 != side.keys.size() ? side.keys[key + 1] : side.versions.size();
            for (std::size_t version = begin; version != end; ++version) [[maybe_unused]]
                st::status_t const placed = joined.versions.push_back(st::assume_reserved,
                                                                      strings_version_t {side.versions[version]});
        };
        for (std::size_t row = 0; row != rows; ++row) {
            [[maybe_unused]] st::status_t const noted = joined.keys.push_back(
                st::assume_reserved, static_cast<std::uint32_t>(joined.versions.size()));
            if (newer_positions[row] != absent_position_k) append_key(newer, newer_positions[row]);
            if (older_positions[row] != absent_position_k) append_key(older, older_positions[row]);
        }
        newer = std::move(joined);
        return {};
    }

    /** Adds one to @p depths for every version of a tie of equal heads, @p depth heads deep, among
     *  the versions from @p first to @p last that a run would give a layer, and recurses into the
     *  ties nested in it. */
    static void count_layers_(std::span<sorted_run_t const* const> sources, std::span<strings_version_t const> versions,
                              std::size_t first, std::size_t last, std::uint32_t depth,
                              std::span<std::uint32_t> depths) noexcept {
        std::size_t const skipped = std::size_t {depth} * strings::head_prefix_bytes_k;
        auto head_of_version = [&](std::size_t index) noexcept {
            return strings::head_of(key_of_(sources, versions[index]).substr(skipped));
        };
        for (std::size_t begin = first; begin != last;) {
            st::key128_t const head = head_of_version(begin);
            std::size_t end = begin + 1;
            while (end != last && head_of_version(end) == head) ++end;
            if (end - begin > sorted_run_t::strings_layout_t::keys_per_row_k &&
                key_of_(sources, versions[begin]) != key_of_(sources, versions[end - 1])) {
                for (std::size_t index = begin; index != end; ++index) ++depths[index];
                count_layers_(sources, versions, begin, end, depth + 1, depths);
            }
            begin = end;
        }
    }

    /** Lists the versions of @p sources that survive a merge of strings, in merged order: on the
     *  device compactor where @p request places the merge, and by unions of their keys on the host
     *  otherwise or where the device refuses it. */
    static status_t strings_survivors_(std::span<sorted_run_t const* const> sources, merge_request_t const& request,
                                       st::vector<strings_version_t>& survivors) noexcept {
        std::uint32_t const upper_count = static_cast<std::uint32_t>(request.upper.size());
        compaction_kind_t const kind = kind_of_(request.upper_order, request.depth);
        std::uint64_t records = 0;
        for (sorted_run_t const* source : sources) records += source->population_count();
        cuda_compactor_t* const* const device = std::get_if<cuda_compactor_t*>(&request.compactor);
        if (choose_compactor(request.placement, device != nullptr ? 1 : 0, records) == compactor_choice_t::device_k) {
            if (device == nullptr) return {status_code_t::unsupported_k, "No device compactor serves this database"};
            st::vector<strings_run_view> views;
            if (views.resize(sources.size()) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A merge could not track its source runs"};
            for (std::size_t index = 0; index != sources.size(); ++index)
                views[index] = strings_run_view {&sources[index]->keys(), sources[index]->key_offsets(),
                                                 sources[index]->key_tape(), &sources[index]->stamps(),
                                                 &sources[index]->kinds()};
            st::status_t const ranked = (*device)->merge_strings(kind, std::span<strings_run_view const> {views},
                                                                 upper_count, request.stamps, survivors);
            if (ranked != st::status_t::invalid_argument_k && ranked != st::status_t::out_of_memory_heap_k)
                return compaction_status(ranked);
        }
        return host_strings_survivors_(sources, upper_count, kind, request.stamps, request.cancellation, survivors);
    }

    /** Lists the versions of @p sources that survive, as @c strings_survivors_ does, on the calling
     *  thread: the upper runs folded newest first, then the lower runs joined, by key unions. */
    static status_t host_strings_survivors_(std::span<sorted_run_t const* const> sources, std::uint32_t upper_count,
                                            compaction_kind_t kind, compaction_stamps_t const& stamps,
                                            cancellation_t const* cancellation,
                                            st::vector<strings_version_t>& survivors) noexcept {
        // The newest upper run is the first side, and each older one folds in after it, the
        // lower level last.
        string_side_t merged_side;
        if (status_t const first = side_of_(sources, upper_count - 1, upper_count, merged_side); first.failed())
            return first;
        for (std::uint32_t source = upper_count - 1; source-- != 0;) {
            if (stopped_(cancellation)) return cancelled_status_();
            string_side_t older;
            if (status_t const listed_side = side_of_(sources, source, source + 1, older); listed_side.failed())
                return listed_side;
            if (status_t const folded = fold_sides_(sources, merged_side, older); folded.failed()) return folded;
        }
        if (upper_count != sources.size()) {
            if (stopped_(cancellation)) return cancelled_status_();
            string_side_t lower;
            if (status_t const listed_side = side_of_(sources, upper_count, static_cast<std::uint32_t>(sources.size()),
                                                      lower);
                listed_side.failed())
                return listed_side;
            if (status_t const folded = fold_sides_(sources, merged_side, lower); folded.failed()) return folded;
        }

        // The versions that survive, as the compaction of fixed-width keys decides it for each
        // key in turn.
        if (survivors.reserve(merged_side.versions.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A merge of strings could not list its survivors"};
        located_key_slot<key_slot_t> previous;
        for (std::size_t key = 0; key != merged_side.keys.size(); ++key) {
            std::size_t const begin = merged_side.keys[key];
            std::size_t const end = key + 1 != merged_side.keys.size() ? merged_side.keys[key + 1]
                                                                       : merged_side.versions.size();
            for (std::size_t index = begin; index != end; ++index) {
                strings_version_t const version = merged_side.versions[index];
                sorted_run_t const& source = *sources[version.source];
                located_key_slot<key_slot_t> current;
                current.key_slot.stored_key = source.head(version.position);
                current.sorted_run = version.source;
                current.kind = source.kind(version.position);
                current.stamp = source.stamp(version.position);
                current.position = version.position;
                if (survives(index != begin ? &previous : nullptr, current, kind, stamps)) [[maybe_unused]]
                    st::status_t const kept = survivors.push_back(st::assume_reserved, strings_version_t {version});
                previous = current;
            }
        }
        return {};
    }

    /** Merges runs of strings: the upper runs folded newest first and then the lower runs,
     *  concatenated, as one side, by unions of their keys, keeping the versions @p request's stamps
     *  keep, written as new runs cut to equal sizes that never split the versions of one key. */
    static expected<merged_runs_t> merge_strings_(volume_t& volume, run_pool_t& runs, io_channel_t& writing_channel,
                                                  merge_request_t const& request) noexcept {
        st::vector<sorted_run_t const*> listed;
        if (listed.resize(request.upper.size() + request.lower.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A merge could not track its source runs"};
        for (std::size_t index = 0; index != request.upper.size(); ++index) listed[index] = request.upper[index];
        for (std::size_t index = 0; index != request.lower.size(); ++index)
            listed[request.upper.size() + index] = request.lower[index];
        std::span<sorted_run_t const* const> const sources {listed};

        st::vector<strings_version_t> survivors;
        if (status_t const kept = strings_survivors_(sources, request, survivors); kept.failed()) return kept;

        // Each survivor plans its key, and a share of the layers it lands in, so the cut runs
        // fit their limit.
        st::vector<std::uint32_t> depths;
        st::vector<std::uint64_t> planned;
        if (depths.resize(survivors.size()) != st::status_t::success_k ||
            planned.resize(survivors.size() + 1) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A merge of strings could not plan its runs"};
        for (std::uint32_t& depth : depths) depth = 0;
        count_layers_(sources, std::span<strings_version_t const> {survivors}, 0, survivors.size(), 0,
                      std::span<std::uint32_t> {depths});
        planned[0] = 0;
        for (std::size_t index = 0; index != survivors.size(); ++index)
            planned[index + 1] = planned[index] + key_of_(sources, survivors[index]).size() +
                                 2 * sizeof(st::key128_t) * depths[index];
        auto const read_prefix = [&planned](std::uint64_t index, std::uint64_t& bytes) noexcept {
            bytes = planned[static_cast<std::size_t>(index)];
            return st::status_t::success_k;
        };
        // Runs split between keys alone, so a level finds every version of a key in one run.
        auto const shares_key = [&](std::uint64_t index, bool& shared) noexcept {
            shared = key_of_(sources, survivors[static_cast<std::size_t>(index)]) ==
                     key_of_(sources, survivors[static_cast<std::size_t>(index) - 1]);
            return st::status_t::success_k;
        };
        st::vector<compacted_sorted_run_t> cuts;
        if (st::status_t const cut = plan_sorted_runs<key_slot_t>(survivors.size(), bounds_of_(request), read_prefix,
                                                                  shares_key, cuts);
            cut != st::status_t::success_k)
            return compaction_status(cut);

        merged_runs_t merged;
        if (merged.reserve(cuts.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A merge could not track the runs it writes"};
        std::size_t first = 0;
        for (compacted_sorted_run_t const& cut : cuts) {
            if (stopped_(request.cancellation)) {
                runs.discard(volume, std::span<run_handle_t const> {merged});
                return cancelled_status_();
            }
            std::span<strings_version_t const> const run_versions =
                std::span<strings_version_t const> {survivors}.subspan(first,
                                                                       static_cast<std::size_t>(cut.key_slots_count));
            auto [written, write_status] = write_strings_run_(volume, writing_channel, sources, run_versions, request);
            if (write_status.failed()) {
                runs.discard(volume, std::span<run_handle_t const> {merged});
                return write_status;
            }
            auto [taken, take_status] = runs.take(std::move(written));
            if (take_status.failed()) {
                volume.deallocate(written.extent());
                runs.discard(volume, std::span<run_handle_t const> {merged});
                return take_status;
            }
            [[maybe_unused]] st::status_t const placed = merged.push_back(std::move(taken));
            first += static_cast<std::size_t>(cut.key_slots_count);
        }
        return merged;
    }

    /** Writes @p versions of @p sources, in order, as one new run of strings stamped from
     *  @p request's floor. */
    static expected<sorted_run_t> write_strings_run_(volume_t& volume, io_channel_t& writing_channel,
                                                     std::span<sorted_run_t const* const> sources,
                                                     std::span<strings_version_t const> versions,
                                                     merge_request_t const& request) noexcept {
        std::size_t const count = versions.size();
        std::uint64_t tails_bytes = 0;
        for (strings_version_t const version : versions)
            tails_bytes += strings::tail_of(key_of_(sources, version)).size();
        if (tails_bytes > limit<std::uint32_t>::max())
            return status_t {status_code_t::capacity_exhausted_k, "A merged run's keys outgrow their offsets"};
        st::vector<key_slot_t> key_slots;
        st::vector<std::uint64_t> stamps;
        st::vector<std::uint8_t> kinds;
        st::vector<std::uint32_t> tail_offsets;
        st::vector<byte_t> tails;
        if (key_slots.resize(count) != st::status_t::success_k || stamps.resize(count) != st::status_t::success_k ||
            kinds.resize(count) != st::status_t::success_k ||
            tail_offsets.resize(count + 1) != st::status_t::success_k ||
            tails.resize(static_cast<std::size_t>(tails_bytes)) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A merge of strings could not lay out a run"};
        std::uint32_t written = 0;
        for (std::size_t index = 0; index != count; ++index) {
            sorted_run_t const& source = *sources[versions[index].source];
            std::uint64_t const position = versions[index].position;
            std::string_view const tail = strings::tail_of(source.key(position));
            key_slot_t slot {};
            slot.stored_key = source.head(position);
            std::memcpy(slot.data, source.value(position).data(), key_slot_t::size);
            key_slots[index] = slot;
            stamps[index] = std::max(source.stamp(position), request.stamps.floor);
            kinds[index] = static_cast<std::uint8_t>(source.kind(position));
            tail_offsets[index] = written;
            if (!tail.empty()) std::memcpy(tails.data() + written, tail.data(), tail.size());
            written += static_cast<std::uint32_t>(tail.size());
        }
        tail_offsets[count] = written;
        key_tails_column_t const column = tails_bytes != 0
                                              ? key_tails_column_t {std::span<std::uint32_t const> {tail_offsets},
                                                                    std::span<byte_t const> {tails}}
                                              : key_tails_column_t {};
        return sorted_run_t::write(volume, writing_channel, request.collection, uuid_generator(),
                                   std::span<key_slot_t const> {key_slots}, std::span<std::uint64_t const> {stamps},
                                   std::span<std::uint8_t const> {kinds}, request.stamps.floor, {}, {},
                                   request.value_max_size, column);
    }

#pragma endregion Strings Merge

#pragma region Private Types

    /** One run of the compaction's plan, with where its cells, survivors and copy ranges start in the output. */
    struct planned_run_t {

        /** Key slots the run holds, padding cells excluded. */
        std::uint64_t key_slots_count = 0;

        /** Bytes of the values its key slots locate, before the section's alignment padding. */
        std::uint64_t stored_values_bytes = 0;

        /** Where this run's survivors start in the compaction's cells, stamps,
         *  kinds and offsets. */
        std::uint64_t first_key_slot = 0;

        /** Where this run's value copy ranges start in the compaction's. */
        std::uint64_t first_copy_range = 0;

        /** Value copy ranges filling this run's stored values section. */
        std::uint64_t copy_ranges_count = 0;
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
        // The trailer packs at most a stamp and a kind per key slot, which its cells outweigh, and
        // one page of headers and padding, so the plan cuts the cells and values short enough to
        // leave it room.
        std::uint64_t const trailer_per_key_slot = sizeof(std::uint64_t) + sizeof(std::uint8_t);
        std::uint64_t const planned = request.run_bytes_limit > portable_alignment_k
                                          ? request.run_bytes_limit - portable_alignment_k
                                          : 1;
        sorted_run_bounds_t bounds;
        bounds.file_bytes_limit = planned / (sizeof(key_slot_t) + trailer_per_key_slot) * sizeof(key_slot_t);
        bounds.header_bytes = sorted_run_layout_t::header_bytes();
        bounds.alignment_bytes = portable_alignment_k;
        bounds.value_stride = is_fixed(value_layout_k) ? request.value_max_size : 0;
        return bounds;
    }

    /** Points a view of @p views at every source run's own cells, which the compactor reads in place. */
    static status_t point_views_(std::span<sorted_run_t const* const> upper, std::span<sorted_run_t const* const> lower,
                                 st::vector<sorted_run_t const*>& sources,
                                 st::vector<sorted_run_index_view<key_slot_t>>& views) noexcept {
        std::size_t const runs = upper.size() + lower.size();
        if (sources.resize(runs) != st::status_t::success_k || views.resize(runs) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A merge could not track its source runs"};
        for (std::size_t index = 0; index != upper.size(); ++index) sources[index] = upper[index];
        for (std::size_t index = 0; index != lower.size(); ++index) sources[upper.size() + index] = lower[index];

        for (std::size_t index = 0; index != runs; ++index) {
            sorted_run_t const& run = *sources[index];
            key_slot_t const* cells = nullptr;
            if constexpr (sorted_run_t::keeps_cells_k) cells = run.cells().data();
            views[index] = sorted_run_index_view<key_slot_t> {
                cells, run.population_count(), &run.stamps(), &run.kinds(), {}, run.value_offsets(), &run.keys()};
        }
        return {};
    }

    /** Where the sections of @p run sit on its volume, and the stamp an ingest gave it, which a device compactor
     *  reads. */
    [[nodiscard]] static sorted_run_extents_t extents_of_(sorted_run_t const& run) noexcept {
        medium_extent_t const& blob = run.extent();
        if (blob.medium.medium_id == utilities::registered_medium_id_t::unregistered_k)
            return {.assigned_stamp = run.assigned_stamp()};
        sorted_run_header_t const& header = run.header();
        std::uint64_t const count = header.key_slots_count;
        std::uint64_t const offsets_bytes = round_up_to_multiple(sorted_run_layout_t::value_offsets_bytes(count),
                                                                 portable_alignment_k);
        return sorted_run_extents_t {
            std::uint32_t {blob.data_directory},
            blob.slice(sorted_run_layout_t::index_offset(), sorted_run_layout_t::index_section_bytes(count)),
            blob.slice(sorted_run_layout_t::value_offsets_offset(count), offsets_bytes),
            blob.slice(sorted_run_layout_t::stamps_offset(count, header.stored_values_bytes),
                       sorted_run_layout_t::trailer_bytes(header.stamps_bytes, header.kinds_bytes)),
            header.stamps_bytes,
            run.assigned_stamp(),
            blob.slice(sorted_run_layout_t::keys_offset(count), sorted_run_layout_t::keys_section_bytes(count)),
            sorted_run_layout_t::keys_leaves_offset(count),
            run.content_id()};
    }

    /** Counts the key slots @p upper and @p lower merge, and tells whether any of their runs holds a change. */
    [[nodiscard]] static std::uint64_t merged_count_(std::span<sorted_run_index_view<key_slot_t> const> upper,
                                                     std::span<sorted_run_index_view<key_slot_t> const> lower,
                                                     bool& changes) noexcept {
        std::uint64_t merged = 0;
        changes = false;
        for (std::size_t index = 0; index != upper.size(); ++index) {
            merged += upper[index].key_slots_count;
            changes |= holds_changes(upper[index].kinds);
        }
        for (std::size_t index = 0; index != lower.size(); ++index) {
            merged += lower[index].key_slots_count;
            changes |= holds_changes(lower[index].kinds);
        }
        return merged;
    }

    /** Compacts on the device compactor @p compactor names when @p placement asks for a device and no run holds a
     *  change, or one does and @p device_fold composes changes, reading the sections of @p sources, and on the calling
     *  thread otherwise, where runs carrying tails and merges outgrowing the device's scratch merge. */
    template <typename fold_type_, typename device_fold_type_, typename value_reader_type_>
    static status_t run_compaction_(compaction_kind_t kind, std::span<sorted_run_index_view<key_slot_t> const> upper,
                                    std::span<sorted_run_index_view<key_slot_t> const> lower,
                                    [[maybe_unused]] std::span<sorted_run_t const* const> sources,
                                    sorted_run_bounds_t const& bounds, compaction_stamps_t const& stamps,
                                    compaction_placement_t placement, compactor_ref_t const& compactor,
                                    fold_type_ const& fold, [[maybe_unused]] device_fold_type_ const& device_fold,
                                    value_reader_type_&& read_value, compaction_output_t& output) noexcept {
        bool changes = false;
        std::uint64_t const merged = merged_count_(upper, lower, changes);
        cuda_compactor_t* const* const device = std::get_if<cuda_compactor_t*>(&compactor);
        if (choose_compactor(placement, device != nullptr ? 1 : 0, merged) == compactor_choice_t::device_k) {
            if (device == nullptr) return {status_code_t::unsupported_k, "No device compactor serves this database"};
            // No device compaction is compiled for key slots carrying tails.
            if constexpr (!tailed_key_slot_concept<key_slot_t>) {
                constexpr bool device_folds_k = !std::same_as<device_fold_type_, no_device_fold_t>;
                st::vector<sorted_run_extents_t> extents;
                if (extents.resize(sources.size()) != st::status_t::success_k)
                    return {status_code_t::out_of_memory_k, "A merge could not track where its runs sit"};
                for (std::size_t index = 0; index != sources.size(); ++index)
                    extents[index] = extents_of_(*sources[index]);
                st::status_t compacted = st::status_t::invalid_argument_k;
                if constexpr (device_folds_k)
                    compacted = (*device)->compact(kind, upper, lower, extents, bounds, stamps, fold, device_fold,
                                                   read_value, output);
                // No run holds a change here, so the compaction built for the last write serves every fold
                else if (!changes)
                    compacted = (*device)->compact(kind, upper, lower, extents, bounds, stamps,
                                                   utilities::last_write_fold_t {}, no_device_fold_t {},
                                                   located_value_reader<key_slot_t> {}, output);
                if (compacted != st::status_t::invalid_argument_k && compacted != st::status_t::out_of_memory_heap_k)
                    return compaction_status(compacted);
            }
        }
        return compaction_status(
            host_compactor_t {}.compact(kind, upper, lower, bounds, stamps, fold, read_value, output));
    }

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

    /** Allocates an extent for the run @p planned describes, writes its sections, and opens it. */
    static expected<sorted_run_t> write_run_(volume_t& volume, io_channel_t& reading_channel,
                                             io_channel_t& writing_channel,
                                             std::span<sorted_run_t const* const> sources,
                                             compaction_output_t const& output, planned_run_t const& planned,
                                             uuid_t const& collection, stored_value_size_t value_max_size,
                                             std::uint64_t floor, cancellation_t const* cancellation) noexcept {
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
        header.collection = collection;
        header.stamps_bytes = column.saved_bytes();
        header.stamps_floor = floor;
        header.newest_stamp = newest;
        header.kinds_bytes = kinds.saved_bytes();

        // The run's own value offsets from its first key slot on, none but for a variable width.
        st::vector<stored_value_offset_t> value_offsets;
        if (!output.value_offsets.empty()) {
            std::uint64_t const begin = output.value_offsets[planned.first_key_slot];
            if (value_offsets.resize(static_cast<std::size_t>(planned.key_slots_count + 1)) != st::status_t::success_k)
                return status_t {status_code_t::out_of_memory_k, "A merge could not lay out a run's value offsets"};
            for (std::uint64_t index = 0; index != value_offsets.size(); ++index)
                value_offsets[index] = static_cast<stored_value_offset_t>(
                    output.value_offsets[planned.first_key_slot + index] - begin);
        }

        std::span<key_slot_t const> const cells {output.cells.data() + planned.first_key_slot,
                                                 static_cast<std::size_t>(planned.key_slots_count)};
        st::vector<std::uint64_t> keys;
        if (status_t const saved = sorted_run_t::save_keys(cells, keys); saved.failed()) return saved;

        auto [extent, allocate_status] = volume.allocate(content_id, sorted_run_layout_t::blob_bytes(header));
        if (allocate_status.failed()) return allocate_status;

        if (status_t const written = write_sections_(
                volume, reading_channel, writing_channel, sources, output, planned, header, column, kinds, cells,
                std::span<std::uint64_t const> {keys}, std::span<stored_value_offset_t const> {value_offsets}, extent,
                cancellation);
            written.failed()) {
            volume.deallocate(extent);
            return written;
        }
        expected<sorted_run_t> opened = sorted_run_t::open(volume, writing_channel, extent);
        if (!opened) volume.deallocate(extent);
        return opened;
    }

    /** Writes the header, the cells and the saved tree of their keys, the value offsets, the
     *  gathered values, the stamps and the kinds of one new run, and reaps every write, leaving the
     *  flush to the manifest that names it. */
    static status_t write_sections_(volume_t& volume, io_channel_t& reading_channel, io_channel_t& writing_channel,
                                    std::span<sorted_run_t const* const> sources, compaction_output_t const& output,
                                    planned_run_t const& planned, sorted_run_header_t const& header,
                                    stamps_column_t const& column, kinds_column_t const& kinds,
                                    std::span<key_slot_t const> cells, std::span<std::uint64_t const> keys,
                                    std::span<stored_value_offset_t const> value_offsets, medium_extent_t const& blob,
                                    cancellation_t const* cancellation) noexcept {
        std::uint64_t const block_bytes = round_down_to_multiple(writing_channel.streams().buffer_bytes(),
                                                                 portable_alignment_k);
        if (block_bytes < sorted_run_layout_t::header_bytes())
            return {status_code_t::capacity_exhausted_k, "A channel's streaming block is shorter than a run's header"};

        auto [writer_lease, writer_lease_status] = writing_channel.lease();
        if (writer_lease_status.failed()) return writer_lease_status;
        write_pipeline_t writer {volume.disk(), std::move(writer_lease), blob.medium};
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

        if (status_t const index = sorted_run_t::write_index(writer, blob, header, cells, keys); index.failed())
            return index;
        if (status_t const offsets = sorted_run_t::write_value_offsets(writer, blob, header, value_offsets);
            offsets.failed())
            return offsets;
        if (status_t const values = write_values_(volume, reading_channel, writer, sources, output, planned, header,
                                                  blob, block_bytes, cancellation);
            values.failed())
            return values;
        if (status_t const trailer = sorted_run_t::write_trailer(writer, blob, header, column, kinds); trailer.failed())
            return trailer;
        return writer.poll_all_completions();
    }

    /**
     *  @brief Gathers the run's value copy ranges out of their source runs and the folded bytes, and writes them back
     *      to back.
     *
     *  @note Ranges are read into a scratch buffer a block deep plus one read block, so a range that
     *      straddles the boundary between two written blocks is still read whole exactly once.
     */
    static status_t write_values_(volume_t& volume, io_channel_t& reading_channel, write_pipeline_t& writer,
                                  std::span<sorted_run_t const* const> sources, compaction_output_t const& output,
                                  planned_run_t const& planned, sorted_run_header_t const& header,
                                  medium_extent_t const& blob, std::uint64_t block_bytes,
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
            st::vector<pending_piece_t> pending;
            if (pending.resize(depth) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A merge could not track its reads in flight"};
            st::vector<byte_t> scratch;
            if (scratch.resize(static_cast<std::size_t>(block_bytes + read_block)) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A merge could not hold the values it gathers"};

            auto [reader_lease, reader_lease_status] = reading_channel.lease();
            if (reader_lease_status.failed()) return reader_lease_status;
            read_pipeline_t reader {volume.disk(), std::move(reader_lease), medium_reference_t {}};
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

            std::uint64_t range = planned.first_copy_range;
            std::uint64_t const ranges_end = planned.first_copy_range + planned.copy_ranges_count;
            std::uint64_t chunk_offset = 0;
            std::uint64_t gathered = 0;
            std::uint64_t written = 0;
            request_tag_t tag = 0;
            while (written != section) {
                if (stopped_(cancellation)) {
                    volume.disk().cancel(reading_channel.queue_pair());
                    return cancelled_status_();
                }

                while (gathered < block_bytes && range != ranges_end) {
                    while (gathered < block_bytes && range != ranges_end && reader.can_enqueue()) {
                        value_copy_range_t const& copied = output.value_copy_ranges[range];
                        // Values a fold wrote are copied out of the compaction's own bytes, a read block at a time.
                        if (copied.source_run == sources.size()) {
                            std::uint64_t const length = std::min<std::uint64_t>(read_block,
                                                                                 copied.bytes - chunk_offset);
                            std::memcpy(scratch.data() + gathered,
                                        output.folded.data() + copied.source_begin + chunk_offset,
                                        static_cast<std::size_t>(length));
                            gathered += length;
                            chunk_offset += length;
                            if (chunk_offset == copied.bytes) {
                                ++range;
                                chunk_offset = 0;
                            }
                            continue;
                        }
                        sorted_run_t const& source = *sources[copied.source_run];
                        std::uint64_t const first = sorted_run_layout_t::values_offset(source.population_count()) +
                                                    copied.source_begin;
                        std::uint64_t const aligned = round_down_to_multiple(first, portable_alignment_k);
                        std::uint64_t const skipped = first - aligned;
                        std::uint64_t const covered = round_up_to_multiple(skipped + copied.bytes,
                                                                           portable_alignment_k);
                        extent_t const window = source.extent().slice(aligned, covered);
                        if (window.length != covered)
                            return {status_code_t::malformed_k, "A source run is shorter than the values it copies"};

                        std::uint64_t const length = read_block < covered - chunk_offset ? read_block
                                                                                         : covered - chunk_offset;
                        std::uint64_t const from = chunk_offset > skipped ? chunk_offset : skipped;
                        std::uint64_t const wanted_last = skipped + copied.bytes;
                        std::uint64_t const to = chunk_offset + length < wanted_last ? chunk_offset + length
                                                                                     : wanted_last;
                        pending_piece_t piece;
                        piece.destination = gathered;
                        piece.skipped = static_cast<std::uint32_t>(from - chunk_offset);
                        piece.carried = static_cast<std::uint32_t>(to > from ? to - from : 0);
                        pending[tag % depth] = piece;
                        if (reader
                                .enqueue_read(source.extent().medium, extent_t {window.offset + chunk_offset, length},
                                              tag)
                                .failed())
                            break;
                        ++tag;
                        gathered += piece.carried;
                        chunk_offset += length;
                        if (chunk_offset == covered) {
                            ++range;
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

/** One run of a tree as a manifest names it, which is everything a restore needs to
 *  open it again. */
struct lsm_tree_run_record_t {

    /** The level the run belongs to, zero being the level flushes land on. */
    std::uint32_t level_number = 0;

    /** The run's own identifier, which its header also carries and which a manifest
     *  may leave zero. */
    uuid_t content_id {};

    /** Where the run sits on its volume. */
    medium_extent_t extent {};

    /** The stamp an ingest assigned a run written at stamp zero, since the run keeps its
     *  zeros, or zero. */
    std::uint64_t stamp = 0;
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

    /** Called with @c context after the publication and before the first consumed run is given
     *  back; a failure leaves the consumed runs' bytes to the next mount. */
    status_t (*notify)(void* context) noexcept = nullptr;

    /** What @c notify is handed, which is the owner that records the run set the tree now holds. */
    void* context = nullptr;
};

#pragma endregion Vocabulary

#pragma region Tree

/**
 *  @brief Every level of one collection's LSM tree, as the level set its writer last published.
 *
 *  The tree holds two level sets, the current one and a spare that a publication builds the next
 *  one in; both live in one allocation made at mount, so moving the tree moves no set a reader
 *  walks. A run lives in the database's run pool and counts the sets naming it, so it is given back
 *  once the last of the two lets it go.
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
    using level_t = lsm_level<key_slot_t, volume_t, atomic_reference_, waiter_type_>;

    /** One immutable run of a level. */
    using sorted_run_t = typename level_t::sorted_run_t;

    /** The database's pool every run of this tree lives in. */
    using run_pool_t = typename level_t::run_pool_t;

    /** How a level names one run. */
    using run_handle_t = typename level_t::run_handle_t;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The transport the volume moves bytes through. */
    using disk_t = std::remove_reference_t<decltype(std::declval<volume_type_&>().disk())>;

    /** The compaction that replaces picked runs of two levels with new ones. */
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
        level_set_t(level_set_t&&) noexcept = default;
        level_set_t& operator=(level_set_t&&) noexcept = default;

        /** Levels the set holds, which is at least one once published. */
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

        /** The levels, which name runs the tree counts this set among the namers of. */
        st::vector<level_t> levels_;
    };

#pragma endregion Types

#pragma region Construction

    lsm_tree() noexcept = default;
    lsm_tree(lsm_tree const&) = delete;
    lsm_tree& operator=(lsm_tree const&) = delete;

    lsm_tree(lsm_tree&& other) noexcept
        : volume_(std::exchange(other.volume_, nullptr)), runs_(std::exchange(other.runs_, nullptr)),
          config_(other.config_), slot_bytes_(other.slot_bytes_), level_sets_(std::move(other.level_sets_)),
          current_level_set_(other.current_level_set_), compacting_(other.compacting_) {}

    lsm_tree& operator=(lsm_tree&& other) noexcept {
        if (this == &other) return *this;
        forget_level_sets_();
        volume_ = std::exchange(other.volume_, nullptr);
        runs_ = std::exchange(other.runs_, nullptr);
        config_ = other.config_;
        slot_bytes_ = other.slot_bytes_;
        level_sets_ = std::move(other.level_sets_);
        current_level_set_ = other.current_level_set_;
        compacting_ = other.compacting_;
        return *this;
    }

    /** Lets go of every run without giving its bytes back, since the manifest still names them. */
    ~lsm_tree() noexcept { forget_level_sets_(); }

    /**
     *  @brief An empty tree of one level zero over @p volume, whose runs live in @p runs.
     *  @param[in,out] volume The volume the runs are allocated on, outliving the tree.
     *  @param[in] config The enlarge factor and value sizes of the tree, and the collection
     *      its runs name.
     *  @param[in,out] runs The database's pool the runs live in, outliving the tree.
     *  @param[in] slot_bytes The unit levels and runs are sized by, which only tests shrink.
     */
    static expected<lsm_tree> mount(volume_t& volume, lsm_tree_config_t const& config, run_pool_t& runs,
                                    std::uint64_t slot_bytes = utilities::slot_bytes_k) noexcept {
        // The enlarge factor sizes every level and level zero's run count alike, so a tree without one has no shape
        if (config.level_enlarge_factor < storage_limits_t::level_min_enlarge_factor_k ||
            config.level_enlarge_factor > storage_limits_t::level_max_enlarge_factor_k)
            return status_t {status_code_t::invalid_argument_k, "A tree grows its levels by a factor in its bounds"};
        if (slot_bytes <= utilities::slot_payload_offset_k)
            return status_t {status_code_t::invalid_argument_k, "A slot holds more than its owner block"};
        lsm_tree tree;
        tree.volume_ = &volume;
        tree.runs_ = &runs;
        tree.config_ = config;
        tree.slot_bytes_ = slot_bytes;
        if (tree.level_sets_.resize(2) != st::status_t::success_k ||
            tree.level_sets_[0].levels_.push_back(level_t::of(runs, 0, lsm_level_order_t::overlapping_k)) !=
                st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A tree could not hold its first level"};
        return tree;
    }

    /** Gives every run of every level back to the volume, which destroys the tree's data and which
     *  only a tree no reader reaches and no manifest names may. */
    void destroy() noexcept {
        abandon_(current_());
        abandon_(spare_());
    }

#pragma endregion Construction

#pragma region Lookups

    /** The level set the last publication made current, which its writer builds the next one from. */
    [[nodiscard]] level_set_t const& levels() const noexcept { return level_sets_[current_level_set_]; }

    /** The volume every run of this tree lives on. */
    [[nodiscard]] volume_t& volume() const noexcept { return *volume_; }

    /** The pool every run of this tree lives in. */
    [[nodiscard]] run_pool_t& runs() const noexcept { return *runs_; }

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
     *  @param[in,out] channel The channel the reads are submitted and completed on, held by the calling thread.
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
            st::vector<pending_read_t> pending;
            if (pending.resize(depth) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A tree could not track its reads in flight"};

            auto [reader_lease, reader_lease_status] = channel.lease();
            if (reader_lease_status.failed()) return reader_lease_status;
            read_pipeline_t reader {volume_->disk(), std::move(reader_lease), medium_reference_t {}};
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
            medium_reference_t medium;
            while (request != stored_keys.size() || reader.outstanding() != 0) {
                while (request != stored_keys.size() && reader.can_enqueue()) {
                    if (chunk == 0) {
                        lsm_tree_position_t position;
                        lookup_result_t const found = find(taken, stored_keys[request], position, stamp);
                        results[request] = found;
                        level_t const& level = taken.level(position.level_number);
                        window = found == lookup_result_t::written_k ? level.value_window(position.level_position)
                                                                     : value_window_t {};
                        if (window.extent.length == 0) {
                            if (found == lookup_result_t::written_k)
                                sink(request, std::uint64_t {0}, std::span<byte_t const> {});
                            ++request;
                            continue;
                        }
                        medium = level.run(position.level_position.run_index).extent().medium;
                    }
                    std::uint64_t const at = chunk * block_bytes;
                    std::uint64_t const room = window.extent.length - at;
                    std::uint64_t const length = block_bytes < room ? block_bytes : room;
                    pending[tag % depth] = piece_of_(window, request, at, length);
                    if (reader.enqueue_read(medium, extent_t {window.extent.offset + at, length}, tag).failed()) break;
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
     *  @param[in,out] channel The channel the run is written on, held by the calling thread.
     *  @param[in] key_slots The key slots in ascending order of their keys, as a flush drained them from memory runs.
     *  @param[in] stamps The stamp of every key slot, in the same order.
     *  @param[in] kinds The @c delta_kind_t of every key slot, in the same order.
     *  @param[in] floor The stamp every stamp is raised to at least, since no reader can tell those below it apart.
     *  @param[in] stored_values The values back to back in sorted order, one of @p value_max_size bytes per key slot
     *      for a fixed width.
     *  @param[in] value_offsets Where each value starts inside @p stored_values and one past the last, for a variable
     *      width alone.
     *  @param[in] value_max_size The largest value the collection admits, kept in the run's header.
     *  @param[in] published What to call once the new set is current, which returns once no reader holds the old one.
     *  @param[in] tails The tails of the keys the key slots' heads cut short, in the same order, none for whole keys.
     */
    status_t flush(io_channel_t& channel, std::span<key_slot_t const> key_slots, std::span<std::uint64_t const> stamps,
                   std::span<std::uint8_t const> kinds, std::uint64_t floor, std::span<byte_t const> stored_values,
                   std::span<stored_value_offset_t const> value_offsets, stored_value_size_t value_max_size,
                   lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {},
                   key_tails_column_t const& tails = {}) noexcept {
        auto [written, write_sorted_run_status] = sorted_run_t::write(
            *volume_, channel, config_.collection, uuid_generator(), key_slots, stamps, kinds, floor, stored_values,
            value_offsets, value_max_size, tails);
        if (write_sorted_run_status.failed()) return write_sorted_run_status;
        auto [flushed, take_status] = runs_->take(std::move(written));
        if (take_status.failed()) {
            volume_->deallocate(written.extent());
            return take_status;
        }
        std::span<run_handle_t const> const placed {&flushed, 1};
        if (status_t const prepared = prepare_next_({}, 0, placed); prepared.failed()) {
            runs_->discard(*volume_, placed);
            return prepared;
        }
        return publish_(published);
    }

    /**
     *  @brief Opens the runs @p runs names and publishes them as the level set of this tree.
     *  @param[in,out] channel The channel every run's header and index is read on, held by the calling thread.
     *  @param[in] runs The runs in the order their levels hold them, which is oldest first on level
     *      zero, each carrying the stamp an ingest assigned it or zero.
     *  @param[in] published What to call once the restored set is current, as a flush does.
     *  @return Success, or the status that stopped it with the empty level set still current and
     *      every run left on the volume.
     *
     *  @note Only a tree still holding the one empty level @c mount built takes these, so a reopen
     *      adopts what the medium carries and no call ever merges two level sets into one.
     */
    status_t restore(io_channel_t& channel, std::span<lsm_tree_run_record_t const> runs,
                     lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {}) noexcept {
        if (levels().size() != 1 || !levels().level(0).empty())
            return {status_code_t::invalid_argument_k, "A tree takes its stored runs only while it holds none"};
        if (runs.empty()) return {};

        std::uint32_t deepest = 0;
        for (lsm_tree_run_record_t const& record : runs)
            if (record.level_number > deepest) deepest = record.level_number;

        level_set_t& next = spare_();
        if (next.levels_.reserve(std::size_t {deepest} + 1) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A tree could not hold the levels it restores"};
        for (std::uint32_t number = 0; number <= deepest; ++number) [[maybe_unused]]
            st::status_t const placed = next.levels_.push_back(level_t::of(
                *runs_, number, number == 0 ? lsm_level_order_t::overlapping_k : lsm_level_order_t::sorted_k));

        for (lsm_tree_run_record_t const& record : runs) {
            if (status_t const restored = restore_run_(channel, record, next); restored.failed()) {
                forget_(next);
                return restored;
            }
        }
        if (published.prepare != nullptr)
            if (status_t const prepared = published.prepare(published.context); prepared.failed()) {
                forget_(next);
                return prepared;
            }
        return replace_(published);
    }

    /**
     *  @brief Prepares the next level set naming every run of the current one, and @p runs, sorted
     *      and disjoint, on the level @c ingest_level_ picks for them, which grows empty sorted
     *      levels until it exists.
     *  @param[in,out] runs The runs an ingest wrote, in ascending order of their keys, each taken
     *      into the run pool.
     *  @return Success, or the status that stopped it, with every run it took given back and the
     *      rest left in @p runs.
     *  @note The caller holds what serializes the tree's publications, so the current set holds
     *      still, and calls @c adopt and then @c retire before it prepares again.
     */
    status_t prepare_ingest(std::span<sorted_run_t> runs) noexcept {
        if (runs.empty()) return {status_code_t::invalid_argument_k, "An ingest publishes at least one run"};
        if (!spare_().levels_.empty())
            return {status_code_t::invalid_argument_k, "A tree retires one set before it prepares another"};
        std::uint64_t bytes = 0;
        for (sorted_run_t const& run : runs) bytes += run.size_bytes();
        std::size_t const target = ingest_level_(levels(), runs.front().lower_stored_key(),
                                                 runs.back().upper_stored_key(), bytes);

        st::vector<run_handle_t> taken;
        if (taken.reserve(runs.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A tree could not track the runs an ingest wrote"};
        for (sorted_run_t& run : runs) {
            auto [handle, take_status] = runs_->take(std::move(run));
            if (take_status.failed()) {
                runs_->discard(*volume_, std::span<run_handle_t const> {taken});
                return take_status;
            }
            [[maybe_unused]] st::status_t const placed = taken.push_back(std::move(handle));
        }
        if (status_t const prepared = prepare_next_({}, target, taken); prepared.failed()) {
            runs_->discard(*volume_, std::span<run_handle_t const> {taken});
            return prepared;
        }
        return {};
    }

    /** Makes the set @c prepare_ingest prepared current; the set it replaced still names every run,
     *  so a reader holding it reads on until the caller calls @c retire. */
    void adopt() noexcept { current_level_set_ ^= 1; }

    /** Empties the set @c adopt replaced, once no reader can hold it. */
    void retire() noexcept { abandon_(spare_()); }

    /** Whether any level of @p taken is over the bytes its number admits. */
    [[nodiscard]] bool needs_compaction(level_set_t const& taken) const noexcept {
        return overfull_(taken) != taken.size();
    }

    /**
     *  @brief Merges the runs of the shallowest level over its bounds that cost least with the
     *      runs below they overlap, or moves one down untouched when it overlaps none, and
     *      publishes the result.
     *  @param[in,out] reading_channel The channel the merge gathers source values on, held by the calling thread.
     *  @param[in,out] writing_channel The channel the merge writes its new runs on, which is not @p reading_channel.
     *  @param[in] placement Where the compaction runs, which a collection leaves automatic.
     *  @param[in] compactor The compactor the database picked at open: the calling thread, or its warm device
     *      compactor.
     *  @param[in] cancellation The word the merge reads between blocks, or null for a merge nobody stops.
     *  @param[in] fold How the collection's changes compose and apply.
     *  @param[in] device_fold How a device composes the collection's changes, or @c no_device_fold_t where none does,
     *      which keeps every merge holding a change on the host.
     *  @param[in] published What to call once the merged set is current, which returns once no reader holds the set
     *      it replaced and before a consumed run is given back.
     *  @param[in] stamps Which older versions survive for readers and for a mount, and the floor the new runs count
     *      from, where the default keeps the newest version of every key alone.
     *  @return What the pass did, or the status that stopped it with the current level set untouched.
     */
    template <utilities::fold_concept fold_type_, typename device_fold_type_>
    expected<compaction_outcome_t> compact(
        io_channel_t& reading_channel, io_channel_t& writing_channel, compaction_placement_t placement,
        compactor_ref_t const& compactor, cancellation_t const* cancellation, fold_type_ const& fold,
        device_fold_type_ const& device_fold,
        lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {},
        compaction_stamps_t const& stamps = compaction_stamps_t {}) noexcept {
        if (!claim_compaction_()) return compaction_outcome_t::nothing_to_compact_k;

        expected<compaction_outcome_t> const outcome = compact_once_(reading_channel, writing_channel, placement,
                                                                     compactor, cancellation, fold, device_fold,
                                                                     published, stamps, merge_choice_t::overfull_k);
        atomic_reference_<std::int32_t> {compacting_}.store(static_cast<std::int32_t>(compaction_state_t::idle_k),
                                                            std::memory_order_release);
        return outcome;
    }

    /**
     *  @brief Drops every version stamped above @p ceiling, merging down each run that holds one
     *      until none does.
     *
     *  A mount runs this before anything draws a stamp: a crash may leave runs holding commits the
     *  manifest it found never promised, and those land nowhere else, so dropping them all leaves
     *  only commits that landed whole.
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
        while (result.succeeded() && above_(levels(), ceiling) != levels().size()) {
            // A mount truncates on the host, so no device composes a change
            expected<compaction_outcome_t> const outcome = compact_once_(
                reading_channel, writing_channel, compaction_placement_t::host_k, {}, nullptr, fold,
                no_device_fold_t {}, published, stamps, merge_choice_t::above_ceiling_k);
            if (!outcome) result = outcome.status();
        }
        atomic_reference_<std::int32_t> {compacting_}.store(static_cast<std::int32_t>(compaction_state_t::idle_k),
                                                            std::memory_order_release);
        return result;
    }

#pragma endregion Publication

  private:

#pragma region Private Types

    /** Which runs one merge pass takes down into the level below. */
    enum class merge_choice_t : std::uint8_t {

        /** Runs of the shallowest level over the bytes its number admits. */
        overfull_k,

        /** Runs of the shallowest level holding a version above the ceiling of
         *  the pass's stamps. */
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

    /** The runs one pass consumes: those of the upper level first, then those
     *  below they overlap. */
    struct picked_runs_t {

        /** The picked runs, the upper level's oldest first or ascending, then the
         *  lower level's ascending. */
        st::vector<sorted_run_t const*> runs;

        /** How many of @c runs belong to the upper level. */
        std::size_t upper_count = 0;

        /** The least key any picked run holds. */
        stored_key_t lower_stored_key = limit<stored_key_t>::max();

        /** The greatest key any picked run holds. */
        stored_key_t upper_stored_key = limit<stored_key_t>::min();

        /** The upper level's picked runs. */
        [[nodiscard]] std::span<sorted_run_t const* const> upper() const noexcept {
            return std::span<sorted_run_t const* const> {runs}.first(upper_count);
        }

        /** The lower level's picked runs. */
        [[nodiscard]] std::span<sorted_run_t const* const> lower() const noexcept {
            return std::span<sorted_run_t const* const> {runs}.subspan(upper_count);
        }
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

    /** The set readers are handed. */
    [[nodiscard]] level_set_t& current_() noexcept { return level_sets_[current_level_set_]; }

    /** The set the next publication is built in, which is empty between publications. */
    [[nodiscard]] level_set_t& spare_() noexcept { return level_sets_[current_level_set_ ^ 1]; }

    /** Counts @p levels among the sets naming each of its runs. */
    void name_(level_set_t& levels) noexcept {
        for (level_t const& level : levels.levels_)
            for (run_handle_t const handle : level.runs()) ++runs_->at(handle)->level_sets_naming;
    }

    /** Empties @p levels, giving back the bytes of every run no other set names and vacating it. */
    void abandon_(level_set_t& levels) noexcept {
        for (level_t const& level : levels.levels_)
            for (run_handle_t const handle : level.runs())
                if (sorted_run_t* const run = runs_->at(handle); run && --run->level_sets_naming == 0) {
                    volume_->deallocate(run->extent());
                    // Readers of its last naming set left before the retire, so this generation
                    // step races none.
                    runs_->vacate(handle);
                }
        levels.levels_.clear();
    }

    /** Empties @p levels, vacating every run no other set names and leaving its bytes to the
     *  manifest that names it. */
    void forget_(level_set_t& levels) noexcept {
        for (level_t const& level : levels.levels_)
            for (run_handle_t const handle : level.runs())
                if (sorted_run_t* const run = runs_->at(handle); run && --run->level_sets_naming == 0)
                    runs_->vacate(handle); // Its last set's readers already left, as in abandon_.
        levels.levels_.clear();
    }

    /** Forgets both sets, which a tree going away does while a manifest still names its runs. */
    void forget_level_sets_() noexcept {
        for (level_set_t& levels : level_sets_) forget_(levels);
    }

    /** Opens the run @p record names into its level of @p next, taking the stamp an ingest assigned
     *  it again, and counts @p next among its namers. */
    status_t restore_run_(io_channel_t& channel, lsm_tree_run_record_t const& record, level_set_t& next) noexcept {
        auto [opened, open_sorted_run_status] = sorted_run_t::open(*volume_, channel, record.extent);
        if (open_sorted_run_status.failed()) return open_sorted_run_status;
        if (record.stamp != 0)
            if (status_t const stamped = opened.assign_stamp(record.stamp); stamped.failed()) return stamped;
        auto [handle, take_status] = runs_->take(std::move(opened));
        if (take_status.failed()) return take_status;
        if (status_t const inserted = next.levels_[record.level_number].insert_run(handle); inserted.failed()) {
            runs_->vacate(handle);
            return inserted;
        }
        ++runs_->at(handle)->level_sets_naming;
        return {};
    }

    /**
     *  @brief Builds the spare set from the current one, leaving out @p consumed and naming
     *      @p placed on level @p number, which grows empty sorted levels until it exists, then
     *      counts the spare among their namers.
     *  @return Success, or the status that stopped it with the spare empty and @p placed for the
     *      caller to give back.
     */
    status_t prepare_next_(std::span<sorted_run_t const* const> consumed, std::size_t number,
                           std::span<run_handle_t const> placed) noexcept {
        level_set_t const& source = levels();
        level_set_t& next = spare_();
        std::size_t const deepest = !placed.empty() && number >= source.size() ? number + 1 : source.size();
        if (next.levels_.reserve(deepest) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A tree could not prepare its next level set"};
        for (std::size_t index = 0; index != deepest; ++index) {
            lsm_level_order_t const order = index == 0 ? lsm_level_order_t::overlapping_k : lsm_level_order_t::sorted_k;
            [[maybe_unused]] st::status_t const pushed = next.levels_.push_back(
                level_t::of(*runs_, static_cast<std::uint32_t>(index), order));
            if (index >= source.size()) continue;
            if (status_t const carried = next.levels_[index].carry(source.level(index), consumed); carried.failed()) {
                next.levels_.clear();
                return carried;
            }
        }
        if (!placed.empty()) {
            level_t& target = next.levels_[number];
            if (status_t const reserved = target.reserve(placed.size()); reserved.failed()) {
                next.levels_.clear();
                return reserved;
            }
            for (run_handle_t const handle : placed)
                if (status_t const inserted = target.insert_run(handle); inserted.failed()) {
                    next.levels_.clear();
                    return inserted;
                }
        }
        name_(next);
        return {};
    }

    /** Asks @p published to prepare for the spare set and publishes it, or gives back what only
     *  the spare names. */
    status_t publish_(lsm_tree_publication_hook_t const& published) noexcept {
        if (published.prepare != nullptr)
            if (status_t const prepared = published.prepare(published.context); prepared.failed()) {
                abandon_(spare_());
                return prepared;
            }
        return replace_(published);
    }

    /**
     *  @brief Makes the spare set current, calls @p published, then empties the set it replaced.
     *
     *  The hook publishes the new set to readers and returns only once none is left in the old
     *  one, so the runs only the old set named are unreachable by then; their bytes go back to
     *  the volume only once the hook recorded the new set, and a failed record leaves them to
     *  the next mount.
     */
    status_t replace_(lsm_tree_publication_hook_t const& published) noexcept {
        current_level_set_ ^= 1;
        status_t const recorded = published.notify != nullptr ? published.notify(published.context) : status_t {};
        if (recorded.failed()) forget_(spare_());
        else abandon_(spare_());
        return recorded;
    }

    /** Bytes level @p number of @p levels admits: one slot times F to the level's number at most,
     *  and above the deepest level that level's bytes divided by F per level between them, but
     *  never under one slot. */
    [[nodiscard]] std::uint64_t level_bytes_(level_set_t const& levels, std::size_t number) const noexcept {
        std::uint64_t const factor = config_.level_enlarge_factor;
        std::uint64_t ceiling = slot_bytes_;
        for (std::size_t step = 0; step != number; ++step) {
            if (ceiling > limit<std::uint64_t>::max() / factor) return limit<std::uint64_t>::max();
            ceiling *= factor;
        }
        std::size_t const deepest = levels.size() - 1;
        if (number >= deepest) return ceiling;
        std::uint64_t tracked = levels.level(deepest).size_bytes();
        for (std::size_t step = number; step != deepest; ++step) tracked /= factor;
        return tracked < slot_bytes_ ? slot_bytes_ : tracked > ceiling ? ceiling : tracked;
    }

    /** The level an ingest of @p bytes of keys from @p lower to @p upper lands on, so no shallower level holds a key
     *  of that range: level zero when one of the first two levels overlaps it, and otherwise the shallowest sorted
     *  level above the first overlapping one whose bytes fit, that one's parent when none does, or a level past the
     *  deepest when nothing overlaps. */
    [[nodiscard]] std::size_t ingest_level_(level_set_t const& levels, stored_key_t lower, stored_key_t upper,
                                            std::uint64_t bytes) const noexcept {
        std::size_t overlapping = 0;
        for (; overlapping != levels.size(); ++overlapping) {
            level_t const& level = levels.level(overlapping);
            bool overlaps = false;
            for (std::size_t index = 0; !overlaps && index != level.size(); ++index)
                overlaps = !(level.run(index).upper_stored_key() < lower ||
                             upper < level.run(index).lower_stored_key());
            if (overlaps) break;
        }
        if (overlapping != levels.size() && overlapping <= 1) return 0;
        for (std::size_t number = 1;; ++number) {
            if (overlapping != levels.size() && number + 1 == overlapping) return number;
            std::uint64_t const held = number < levels.size() ? levels.level(number).size_bytes() : 0;
            std::uint64_t const admitted = level_bytes_(levels, number);
            if (held + bytes <= admitted || admitted == limit<std::uint64_t>::max()) return number;
        }
    }

    /** The shallowest level of @p levels that is over its bounds, or the level count when none is:
     *  level zero at F runs or one slot of bytes, and every deeper one over @c level_bytes_. */
    [[nodiscard]] std::size_t overfull_(level_set_t const& levels) const noexcept {
        if (levels.size() != 0 &&
            (levels.level(0).size() >= config_.level_enlarge_factor || levels.level(0).size_bytes() >= slot_bytes_))
            return 0;
        for (std::size_t number = 1; number != levels.size(); ++number)
            if (levels.level(number).size_bytes() > level_bytes_(levels, number)) return number;
        return levels.size();
    }

    /** The shallowest level of @p levels holding a run with a version above @p ceiling, or the level count. */
    [[nodiscard]] static std::size_t above_(level_set_t const& levels, std::uint64_t ceiling) noexcept {
        for (std::size_t number = 0; number != levels.size(); ++number)
            for (std::size_t index = 0; index != levels.level(number).size(); ++index)
                if (levels.level(number).run(index).newest_stamp() > ceiling) return number;
        return levels.size();
    }

    /** Whether @p run holds a key between @p lower and @p upper. */
    [[nodiscard]] static bool overlaps_(sorted_run_t const& run, stored_key_t lower, stored_key_t upper) noexcept {
        return run.population_count() != 0 && !(run.upper_stored_key() < lower || upper < run.lower_stored_key());
    }

    /**
     *  @brief Picks the runs one pass consumes from level @p upper of @p source and the
     *      level below it.
     *
     *  Level zero's runs overlap, so it goes whole. A deeper level gives the run holding a version
     *  above @p ceiling when @p choice asks for one, and otherwise the run whose overlap below
     *  holds the fewest bytes. The lower level then gives every run that overlaps the picked range.
     */
    status_t pick_runs_(level_set_t const& source, std::size_t upper, merge_choice_t choice, std::uint64_t ceiling,
                        picked_runs_t& picked) const noexcept {
        level_t const& from = source.level(upper);
        level_t const* const below = upper + 1 != source.size() ? &source.level(upper + 1) : nullptr;
        std::size_t const lower_runs = below != nullptr ? below->size() : 0;
        if (picked.runs.reserve(from.size() + lower_runs) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A tree could not list the runs a merge consumes"};

        if (upper == 0)
            for (std::size_t index = 0; index != from.size(); ++index) [[maybe_unused]]
                st::status_t const placed = picked.runs.push_back(&from.run(index));
        else {
            std::size_t chosen = 0;
            std::uint64_t cheapest = limit<std::uint64_t>::max();
            std::size_t first_overlapping = 0;
            for (std::size_t index = 0; index != from.size(); ++index) {
                sorted_run_t const& run = from.run(index);
                if (choice == merge_choice_t::above_ceiling_k) {
                    if (run.newest_stamp() <= ceiling) continue;
                    chosen = index;
                    break;
                }
                while (first_overlapping != lower_runs &&
                       below->run(first_overlapping).upper_stored_key() < run.lower_stored_key())
                    ++first_overlapping;
                std::uint64_t overlapped = 0;
                for (std::size_t lower = first_overlapping;
                     lower != lower_runs && !(run.upper_stored_key() < below->run(lower).lower_stored_key()); ++lower)
                    overlapped += below->run(lower).size_bytes();
                if (overlapped >= cheapest) continue;
                cheapest = overlapped;
                chosen = index;
            }
            [[maybe_unused]] st::status_t const placed = picked.runs.push_back(&from.run(chosen));
        }
        picked.upper_count = picked.runs.size();

        stored_key_t range_lower = limit<stored_key_t>::max();
        stored_key_t range_upper = limit<stored_key_t>::min();
        for (sorted_run_t const* run : picked.upper()) {
            if (run->population_count() == 0) continue;
            if (run->lower_stored_key() < range_lower) range_lower = run->lower_stored_key();
            if (range_upper < run->upper_stored_key()) range_upper = run->upper_stored_key();
        }
        picked.lower_stored_key = range_lower;
        picked.upper_stored_key = range_upper;
        for (std::size_t index = 0; index != lower_runs; ++index) {
            sorted_run_t const& run = below->run(index);
            if (!overlaps_(run, range_lower, range_upper)) continue;
            [[maybe_unused]] st::status_t const placed = picked.runs.push_back(&run);
            if (run.lower_stored_key() < picked.lower_stored_key) picked.lower_stored_key = run.lower_stored_key();
            if (picked.upper_stored_key < run.upper_stored_key()) picked.upper_stored_key = run.upper_stored_key();
        }
        return {};
    }

    /** Whether a level of @p source deeper than @p lower holds a key between @p lower_key
     *  and @p upper_key. */
    [[nodiscard]] static bool overlaps_below_(level_set_t const& source, std::size_t lower, stored_key_t lower_key,
                                              stored_key_t upper_key) noexcept {
        for (std::size_t number = lower + 1; number < source.size(); ++number) {
            level_t const& level = source.level(number);
            if (level.empty() || level.upper_stored_key() < lower_key || upper_key < level.lower_stored_key()) continue;
            for (std::size_t index = 0; index != level.size(); ++index)
                if (overlaps_(level.run(index), lower_key, upper_key)) return true;
        }
        return false;
    }

    /** Runs one pass over the runs @p choice picks and publishes it, which the claim around it
     *  keeps to one thread. */
    template <typename fold_type_, typename device_fold_type_>
    expected<compaction_outcome_t> compact_once_(io_channel_t& reading_channel, io_channel_t& writing_channel,
                                                 compaction_placement_t placement, compactor_ref_t const& compactor,
                                                 cancellation_t const* cancellation, fold_type_ const& fold,
                                                 device_fold_type_ const& device_fold,
                                                 lsm_tree_publication_hook_t const& published,
                                                 compaction_stamps_t const& stamps, merge_choice_t choice) noexcept {
        level_set_t const& source = levels();
        std::size_t const upper = choice == merge_choice_t::overfull_k ? overfull_(source)
                                                                       : above_(source, stamps.ceiling);
        if (upper == source.size()) return compaction_outcome_t::nothing_to_compact_k;
        std::size_t const lower = upper + 1;

        picked_runs_t picked;
        if (status_t const pick = pick_runs_(source, upper, choice, stamps.ceiling, picked); pick.failed()) return pick;

        // A run that overlaps nothing below moves down by its handle, and no byte is
        // read or written
        if (picked.lower().empty() && picked.upper_count == 1 && choice == merge_choice_t::overfull_k) {
            run_handle_t const moved = runs_->handle_of(*picked.runs[0]);
            if (status_t const prepared = prepare_next_(picked.upper(), lower,
                                                        std::span<run_handle_t const> {&moved, 1});
                prepared.failed())
                return prepared;
            if (status_t const replaced = publish_(published); replaced.failed()) return replaced;
            return compaction_outcome_t::published_k;
        }

        typename merger_t::merge_request_t request;
        request.upper = picked.upper();
        request.upper_order = source.level(upper).order();
        request.lower = picked.lower();
        request.depth = overlaps_below_(source, lower, picked.lower_stored_key, picked.upper_stored_key)
                            ? lsm_level_depth_t::intermediate_k
                            : lsm_level_depth_t::bottom_k;
        request.collection = config_.collection;
        sorted_run_config_t const& values = config_.lsm_level_0.sorted_run;
        request.value_max_size = is_fixed(value_layout_k) ? values.value_size : values.value_max_size;
        request.run_bytes_limit = slot_bytes_ - utilities::slot_payload_offset_k;
        request.placement = placement;
        request.compactor = compactor;
        request.cancellation = cancellation;
        request.stamps = stamps;

        auto [merged, merge_status] = merger_t::merge(*volume_, *runs_, reading_channel, writing_channel, request, fold,
                                                      device_fold);
        if (merge_status.failed()) return merge_status;
        std::span<run_handle_t const> const placed {merged};
        if (status_t const prepared = prepare_next_(std::span<sorted_run_t const* const> {picked.runs}, lower, placed);
            prepared.failed()) {
            runs_->discard(*volume_, placed);
            return prepared;
        }
        if (status_t const replaced = publish_(published); replaced.failed()) return replaced;
        return compaction_outcome_t::published_k;
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The volume every run of this tree lives on, null until the tree is mounted or once it is moved from. */
    volume_t* volume_ = nullptr;

    /** The database's pool every run of this tree lives in, null until the tree is mounted or once
     *  it is moved from. */
    run_pool_t* runs_ = nullptr;

    /** How fast the bytes a level admits grow with depth, the value sizes of its runs
     *  and their collection. */
    lsm_tree_config_t config_ {};

    /** The unit levels and runs are sized by. */
    std::uint64_t slot_bytes_ = 0;

    /** The current set and the spare, allocated once at mount so a reader's set never moves
     *  with the tree. */
    st::vector<level_set_t> level_sets_;

    /** Which of @c level_sets_ readers are handed. */
    std::size_t current_level_set_ = 0;

    /** A @c compaction_state_t, moved by @c claim_compaction_ alone. */
    std::int32_t compacting_ = 0;

#pragma endregion Private Members
};

#pragma endregion Tree

#pragma region Records

/** Appends a record for every run of @p tree to @p into, level by level and oldest first inside a level. */
template <typename tree_type_>
status_t record_lsm_tree_runs(tree_type_ const& tree, st::vector<lsm_tree_run_record_t>& into) noexcept {
    typename tree_type_::level_set_t const& taken = tree.levels();
    std::size_t counted = 0;
    for (std::size_t number = 0; number != taken.size(); ++number) counted += taken.level(number).size();
    if (into.reserve(into.size() + counted) != st::status_t::success_k)
        return {status_code_t::out_of_memory_k, "No room to name the runs a tree holds"};

    for (std::size_t number = 0; number != taken.size(); ++number) {
        typename tree_type_::level_t const& level = taken.level(number);
        for (std::size_t index = 0; index != level.size(); ++index) {
            typename tree_type_::sorted_run_t const& run = level.run(index);
            lsm_tree_run_record_t record;
            record.level_number = static_cast<std::uint32_t>(number);
            record.content_id = run.content_id();
            record.extent = run.extent();
            record.stamp = run.assigned_stamp();
            [[maybe_unused]] st::status_t const placed = into.push_back(std::move(record));
        }
    }
    return {};
}

#pragma endregion Records

#pragma endregion LSM Tree

} // namespace unum::ustore::blobs
