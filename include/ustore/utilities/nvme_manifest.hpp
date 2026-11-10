/**
 *  @file include/ustore/utilities/nvme_manifest.hpp
 *  @author Ash Vardanian
 *  @date November 4, 2026
 *  @brief The JSON manifest of one database, @c <database>.ustore.json: its collections and every
 *      run file, printed and parsed with yyjson through the database's allocator.
 *
 *  The manifest is written last, as the commit record: a run the manifest does not name is a crash
 *  leftover. On a filesystem a file is named by its @c path, relative to the data directory its
 *  @c medium names; on raw media it is named by @c medium and @c offset. Each run's own header
 *  carries everything needed to read it, so a run lifted out of the set still describes itself.
 *
 *  @code{.json}
 *  {"__metadata__": {"format": "ustore", "version": "1"},
 *   "database": "molecules", "id": "0193f1c0-...", "generation": 42, "durable_stamp": 918273,
 *   "collections": [{"id": "...", "name": "_collections", "store": 0, "value_size": 0, ...}],
 *   "files": [{"path": "molecules-0193f1c2-....ustore", "bytes": 3211264, "collection": 0, "level": 0,
 *              "stamp": 0, "sha256": "9f86d08..."}, {"path": "molecules-0193f1c3-....ustore", "bytes": 1832}]}
 *  @endcode
 *
 *  An archive names the SHA-256 of every run's bytes, which a restore checks every run against.
 */

#pragma once

#include <cstddef> // `std::size_t`, `std::max_align_t`
#include <cstdint> // `std::uint16_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <concepts>    // `std::unsigned_integral`
#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`

#include <yyjson.h> // `yyjson_alc`, `yyjson_read_opts`, `yyjson_mut_write_opts`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/types.hpp" // `status_t`, `status_code_t`, `uuid128_t`, `string_t`, `byte_t`

namespace unum::ustore::utilities {

#pragma region Allocator

/**
 *  @brief yyjson's allocator over a byte allocator of the database, so a manifest is charged
 *      to its budget.
 *
 *  yyjson frees without a size, so every block carries its size in a header one
 *  maximal alignment long.
 */
template <typename allocator_type_>
class json_allocator {
  public:

    explicit json_allocator(allocator_type_ allocator) noexcept : allocator_(std::move(allocator)) {}

    json_allocator(json_allocator const&) = delete;
    json_allocator& operator=(json_allocator const&) = delete;

    /** The table yyjson calls, which borrows this adapter. */
    [[nodiscard]] yyjson_alc table() noexcept { return yyjson_alc {&allocate_, &reallocate_, &deallocate_, this}; }

  private:

    static constexpr std::size_t header_bytes_k = alignof(std::max_align_t);

    static void* allocate_(void* context, std::size_t size) noexcept {
        json_allocator& self = *static_cast<json_allocator*>(context);
        byte_t* const block = self.allocator_.allocate(size + header_bytes_k);
        if (!block) return nullptr;
        std::memcpy(block, &size, sizeof(size));
        return block + header_bytes_k;
    }

    static void deallocate_(void* context, void* pointer) noexcept {
        if (!pointer) return;
        json_allocator& self = *static_cast<json_allocator*>(context);
        byte_t* const block = static_cast<byte_t*>(pointer) - header_bytes_k;
        std::size_t size = 0;
        std::memcpy(&size, block, sizeof(size));
        self.allocator_.deallocate(block, size + header_bytes_k);
    }

    static void* reallocate_(void* context, void* pointer, std::size_t old_size, std::size_t size) noexcept {
        void* const moved = allocate_(context, size);
        if (!moved) return nullptr;
        if (pointer) std::memcpy(moved, pointer, old_size < size ? old_size : size);
        deallocate_(context, pointer);
        return moved;
    }

    allocator_type_ allocator_;
};

#pragma endregion Allocator

#pragma region Manifest

/** What @c manifest_file_t::collection holds for the caller's user data, which belongs
 *  to no collection. */
inline constexpr std::uint32_t manifest_no_collection_k = (std::numeric_limits<std::uint32_t>::max)();

/** How a manifest names its files: by path on a filesystem, by offset on raw media. */
enum class manifest_placement_t : std::uint8_t {
    paths_k,
    offsets_k,
};

/** One collection as the manifest names it, which is all a mount needs to open it again. */
struct manifest_collection_t {

    /** The UUID every commit and the C ABI address the collection by. */
    uuid128_t id;

    /** The name a caller reaches the collection by. */
    string_t name;

    /** The tag of the store a mount opens the collection in. */
    std::uint16_t store = 0;

    /** Bytes every value takes, zero for values that differ in width. */
    std::uint32_t value_size = 0;

    /** Bytes the largest value takes. */
    std::uint32_t value_max_size = 0;

    /** How fast the bytes a level admits grow with its number. */
    std::uint64_t level_enlarge_factor = 0;

    /** Bytes the value cache of the collection holds. */
    std::uint64_t cache_limit = 0;

    /** Bytes the memory runs hold before a flush is due. */
    std::uint64_t memory_runs_max_bytes = 0;

    /** Deltas the memory runs hold before a flush is due. */
    std::uint64_t memory_runs_max_deltas = 0;
};

#pragma pack(push, 1)

/**
 *  @brief What the collections system collection keeps for one collection, its name's bytes
 *      following, so a recovery without a manifest rebuilds every collection's name, store and
 *      shape from it alone.
 *
 *  The manifest keeps the same record as the fast path; a create and a drop publish both at once.
 */
struct collections_record_t {

    /** The tag of the store the collection lives in, which tells its modality. */
    std::uint16_t store_tag = 0;

    /** Written as zero. */
    std::uint16_t padding = 0;

    /** Bytes every value takes, zero for values that differ in width. */
    std::uint32_t value_size = 0;

    /** Bytes the largest value takes. */
    std::uint32_t value_max_size = 0;

    /** Written as zero. */
    std::uint32_t reserved = 0;

    /** How fast the bytes a level admits grow with its number. */
    std::uint64_t level_enlarge_factor = 0;

    /** Bytes the value cache of the collection holds. */
    std::uint64_t cache_limit = 0;

    /** Bytes the memory runs hold before a flush is due. */
    std::uint64_t memory_runs_max_bytes = 0;

    /** Deltas the memory runs hold before a flush is due. */
    std::uint64_t memory_runs_max_deltas = 0;
};

#pragma pack(pop)

static_assert(sizeof(collections_record_t) == 48, "A collections record is 48 packed bytes before the name");

/** The UUID of the reserved collection numbered @p last, all ones but its last byte, which no
 *  minted UUIDv7 spells. */
[[nodiscard]] constexpr uuid128_t reserved_collection_uuid(std::uint8_t last) noexcept {
    uuid128_t identifier;
    for (std::uint8_t& byte : identifier.bytes) byte = 0xFF;
    identifier.bytes[uuid128_t::size_bytes_k - 1] = last;
    return identifier;
}

/** The collections system collection, keeping a @c collections_record_t per collection. */
inline constexpr uuid128_t collections_collection_uuid_k = reserved_collection_uuid(0xFD);

/** The names system collection, a strings collection mapping every collection's name
 *  onto its UUID. */
inline constexpr uuid128_t names_collection_uuid_k = reserved_collection_uuid(0xFE);

/** Bytes the largest value of a collection takes, unless the open of its database
 *  names another bound. */
inline constexpr std::uint32_t engine_value_max_size_k = 1024u * 1024u;

/** Bytes one collection's memory runs hold before a flush is due, and bytes one collection's
 *  value cache holds. */
inline constexpr std::uint64_t engine_collection_bytes_k = 16ull * 1024ull * 1024ull;

/** Deltas one collection's memory runs hold before a flush is due. */
inline constexpr std::uint64_t engine_memory_runs_deltas_k = 64ull * 1024ull;

/** How fast the bytes a level admits grow with its number. */
inline constexpr std::uint64_t engine_level_enlarge_factor_k = 4;

/**
 *  @brief The record a database keeps for the reserved collection @p id, which the open creating it
 *      and a recovery rebuilding it without a manifest both take from here.
 *
 *  Both keep the engine's bounds. The collections system collection holds values of any width up to
 *  @p value_max_size bytes; the names system collection holds one 16-byte key per value and ignores
 *  the bound. The store tag is left to the caller, which derives it from the modality or reads it
 *  from a run's value layout.
 */
[[nodiscard]] inline manifest_collection_t default_reserved_record(uuid128_t const& id,
                                                                   std::uint32_t value_max_size) noexcept {
    manifest_collection_t record;
    record.id = id;
    bool const names = id == names_collection_uuid_k;
    [[maybe_unused]] sz::status_t const named = record.name.assign(names ? string_view_t {"_names"}
                                                                         : string_view_t {"_collections"});
    record.value_size = names ? static_cast<std::uint32_t>(sizeof(uuid128_t)) : 0;
    record.value_max_size = names ? 0 : value_max_size;
    record.level_enlarge_factor = engine_level_enlarge_factor_k;
    record.cache_limit = engine_collection_bytes_k;
    record.memory_runs_max_bytes = engine_collection_bytes_k;
    record.memory_runs_max_deltas = engine_memory_runs_deltas_k;
    return record;
}

/** One run file, or the caller's user data, as the manifest names it. */
struct manifest_file_t {

    /** The run ID a directory volume names the file by, and an archive recognizes a run it
     *  already holds by. */
    uuid128_t run;

    /** First byte of the file inside its medium, on raw media alone. */
    std::uint64_t offset = 0;

    /** Bytes the file spans. */
    std::uint64_t bytes = 0;

    /** The stamp an ingest assigned a run written at stamp zero, or zero. */
    std::uint64_t stamp = 0;

    /** Index of the owning collection in the manifest, or @c manifest_no_collection_k
     *  for user data. */
    std::uint32_t collection = manifest_no_collection_k;

    /** The level the run belongs to, zero being the level flushes land on. */
    std::uint32_t level = 0;

    /** The data directory holding the file. */
    std::uint16_t medium = 0;

    /** The SHA-256 of the file's bytes, which an archive names and a restore checks. */
    sha256_t sha256 {};
};

/**
 *  @brief Everything one database names: its identity, the stamp it is durable up to, its
 *      collections and files.
 *
 *  A @c paths_k file is printed as @c <database>-<run>.ustore and parsed back into its run ID, so
 *  the record stays fixed-width and a manifest of tens of thousands of runs allocates two vectors.
 */
struct manifest_t {

    /** The name the database is opened by, which prefixes every run file. */
    string_t database;

    /** The database's own ID, minted at its first open, which every slot it holds carries. */
    uuid128_t id;

    /** How many manifests the database has published, this one included. */
    std::uint64_t generation = 0;

    /** Every commit at or below this stamp is wholly inside the files named, and a mount drops
     *  every one above. */
    std::uint64_t durable_stamp = 0;

    /** How the files are named. */
    manifest_placement_t placement = manifest_placement_t::paths_k;

    st::vector<manifest_collection_t> collections;
    st::vector<manifest_file_t> files;

    /** Characters the file name of @p run takes, which is the database name, a dash, the ID
     *  and the suffix. */
    [[nodiscard]] std::size_t file_name_length() const noexcept {
        return database.size() + 1 + uuid128_t::printed_length_k + run_suffix_k.size();
    }

    /** Writes @c <database>-<run>.ustore and a terminator into @p into, refusing a
     *  buffer too short. */
    status_t file_name(uuid128_t const& run, std::span<char> into) const noexcept {
        if (into.size() <= file_name_length())
            return {status_code_t::invalid_argument_k, "A run file name outgrew the platform's limit"};
        std::size_t written = database.size();
        std::memcpy(into.data(), database.data(), written);
        into[written++] = '-';
        run.print(std::span<char, uuid128_t::printed_length_k> {into.data() + written, uuid128_t::printed_length_k});
        written += uuid128_t::printed_length_k;
        std::memcpy(into.data() + written, run_suffix_k.data(), run_suffix_k.size());
        written += run_suffix_k.size();
        into[written] = '\0';
        return {};
    }

    /** The run ID @p name carries when it is a run file of this database, or a
     *  zero ID otherwise. */
    [[nodiscard]] uuid128_t run_of(std::string_view name) const noexcept {
        if (name.size() != file_name_length()) return uuid128_t {};
        if (name.substr(0, database.size()) != std::string_view {database}) return uuid128_t {};
        if (name[database.size()] != '-' || !name.ends_with(run_suffix_k)) return uuid128_t {};
        std::span<char const, uuid128_t::printed_length_k> const printed {name.data() + database.size() + 1,
                                                                          uuid128_t::printed_length_k};
        if (!uuid128_t::parses(printed)) return uuid128_t {};
        return uuid128_t::parse(printed);
    }

    /**
     *  @brief Prints this manifest as JSON into @p into, replacing what it held.
     *  @param[in] allocator The database's allocator, which yyjson's document is charged to.
     */
    template <typename allocator_type_>
    status_t print(allocator_type_ const& allocator, st::vector<char>& into) const noexcept {
        json_allocator<allocator_type_> adapter {allocator};
        yyjson_alc const table = adapter.table();
        yyjson_mut_doc* const document = yyjson_mut_doc_new(&table);
        if (!document) return {status_code_t::out_of_memory_k, "No room to print the manifest"};
        status_t const printed = print_into_(document, table, into);
        yyjson_mut_doc_free(document);
        return printed;
    }

    /**
     *  @brief Parses @p json into this manifest, which is left as it was when the text
     *      does not read.
     *  @param[in] json The text, read off the medium; yyjson copies it and never writes into it.
     *  @return Success, or @c malformed_k for a document this layout does not describe.
     */
    template <typename allocator_type_>
    status_t parse(std::span<char> json, allocator_type_ const& allocator) noexcept {
        json_allocator<allocator_type_> adapter {allocator};
        yyjson_alc const table = adapter.table();
        yyjson_read_err error {};
        yyjson_doc* const document = yyjson_read_opts(json.data(), json.size(), YYJSON_READ_NOFLAG, &table, &error);
        if (!document) return {status_code_t::malformed_k, "The manifest is no JSON document"};
        manifest_t parsed;
        status_t const read = parsed.parse_from_(yyjson_doc_get_root(document));
        yyjson_doc_free(document);
        if (read.failed()) return read;
        *this = std::move(parsed);
        return {};
    }

  private:

    static constexpr std::string_view run_suffix_k = ".ustore";

    status_t print_into_(yyjson_mut_doc* document, yyjson_alc const& table, st::vector<char>& into) const noexcept {
        yyjson_mut_val* const root = yyjson_mut_obj(document);
        yyjson_mut_val* const metadata = yyjson_mut_obj(document);
        yyjson_mut_val* const collections_array = yyjson_mut_arr(document);
        yyjson_mut_val* const files_array = yyjson_mut_arr(document);
        if (!root || !metadata || !collections_array || !files_array)
            return {status_code_t::out_of_memory_k, "No room to print the manifest"};
        yyjson_mut_doc_set_root(document, root);
        char printed_id[uuid128_t::printed_length_k];
        id.print(printed_id);
        bool laid = yyjson_mut_obj_add_str(document, metadata, "format", "ustore") &&
                    yyjson_mut_obj_add_str(document, metadata, "version", "1") &&
                    yyjson_mut_obj_add_val(document, root, "__metadata__", metadata) &&
                    yyjson_mut_obj_add_strncpy(document, root, "database", database.data(), database.size()) &&
                    yyjson_mut_obj_add_strncpy(document, root, "id", printed_id, sizeof(printed_id)) &&
                    yyjson_mut_obj_add_uint(document, root, "generation", generation) &&
                    yyjson_mut_obj_add_uint(document, root, "durable_stamp", durable_stamp) &&
                    yyjson_mut_obj_add_val(document, root, "collections", collections_array) &&
                    yyjson_mut_obj_add_val(document, root, "files", files_array);

        for (std::size_t index = 0; laid && index != collections.size(); ++index) {
            manifest_collection_t const& collection = collections[index];
            yyjson_mut_val* const object = yyjson_mut_arr_add_obj(document, collections_array);
            collection.id.print(printed_id);
            laid =
                object && yyjson_mut_obj_add_strncpy(document, object, "id", printed_id, sizeof(printed_id)) &&
                yyjson_mut_obj_add_strncpy(document, object, "name", collection.name.data(), collection.name.size()) &&
                yyjson_mut_obj_add_uint(document, object, "store", collection.store) &&
                yyjson_mut_obj_add_uint(document, object, "value_size", collection.value_size) &&
                yyjson_mut_obj_add_uint(document, object, "value_max_size", collection.value_max_size) &&
                yyjson_mut_obj_add_uint(document, object, "level_enlarge_factor", collection.level_enlarge_factor) &&
                yyjson_mut_obj_add_uint(document, object, "cache_limit", collection.cache_limit) &&
                yyjson_mut_obj_add_uint(document, object, "memory_runs_max_bytes", collection.memory_runs_max_bytes) &&
                yyjson_mut_obj_add_uint(document, object, "memory_runs_max_deltas", collection.memory_runs_max_deltas);
        }

        char name[path_name_capacity_k];
        for (std::size_t index = 0; laid && index != files.size(); ++index) {
            manifest_file_t const& file = files[index];
            yyjson_mut_val* const object = yyjson_mut_arr_add_obj(document, files_array);
            laid = object != nullptr;
            if (laid && placement == manifest_placement_t::paths_k) {
                if (status_t const named = file_name(file.run, name); named.failed()) return named;
                laid = yyjson_mut_obj_add_strncpy(document, object, "path", name, file_name_length()) &&
                       (file.medium == 0 || yyjson_mut_obj_add_uint(document, object, "medium", file.medium));
            }
            else if (laid) {
                file.run.print(printed_id);
                laid = yyjson_mut_obj_add_uint(document, object, "medium", file.medium) &&
                       yyjson_mut_obj_add_uint(document, object, "offset", file.offset) &&
                       (!file.run.is_valid() ||
                        yyjson_mut_obj_add_strncpy(document, object, "run", printed_id, sizeof(printed_id)));
            }
            laid = laid && yyjson_mut_obj_add_uint(document, object, "bytes", file.bytes);
            if (laid && file.collection != manifest_no_collection_k)
                laid = yyjson_mut_obj_add_uint(document, object, "collection", file.collection) &&
                       yyjson_mut_obj_add_uint(document, object, "level", file.level) &&
                       yyjson_mut_obj_add_uint(document, object, "stamp", file.stamp);
            if (laid && !file.sha256.empty()) {
                char printed_digest[sizeof(file.sha256.bytes) * 2];
                constexpr char digits[] = "0123456789abcdef";
                for (std::size_t byte = 0; byte != sizeof(file.sha256.bytes); ++byte) {
                    printed_digest[byte * 2] = digits[file.sha256.bytes[byte] >> 4];
                    printed_digest[byte * 2 + 1] = digits[file.sha256.bytes[byte] & 15];
                }
                laid = yyjson_mut_obj_add_strncpy(document, object, "sha256", printed_digest, sizeof(printed_digest));
            }
        }
        if (!laid) return {status_code_t::out_of_memory_k, "No room to print the manifest"};

        std::size_t length = 0;
        yyjson_write_err error {};
        char* const text = yyjson_mut_write_opts(document, YYJSON_WRITE_NOFLAG, &table, &length, &error);
        if (!text) return {status_code_t::out_of_memory_k, "No room to print the manifest"};
        status_t copied;
        if (into.resize(length) != st::status_t::success_k)
            copied = {status_code_t::out_of_memory_k, "No room to print the manifest"};
        else std::memcpy(into.data(), text, length);
        table.free(table.ctx, text);
        return copied;
    }

    status_t parse_from_(yyjson_val* root) noexcept {
        if (!yyjson_is_obj(root)) return malformed_();
        yyjson_val* const metadata = yyjson_obj_get(root, "__metadata__");
        if (text_of_(yyjson_obj_get(metadata, "format")) != "ustore" ||
            text_of_(yyjson_obj_get(metadata, "version")) != "1")
            return {status_code_t::unsupported_k, "The manifest follows another format"};
        if (!read_(yyjson_obj_get(root, "database"), database) || !read_(yyjson_obj_get(root, "id"), id) ||
            !read_(yyjson_obj_get(root, "generation"), generation) ||
            !read_(yyjson_obj_get(root, "durable_stamp"), durable_stamp))
            return malformed_();

        yyjson_val* const collections_array = yyjson_obj_get(root, "collections");
        yyjson_val* const files_array = yyjson_obj_get(root, "files");
        if (!yyjson_is_arr(collections_array) || !yyjson_is_arr(files_array)) return malformed_();
        if (collections.reserve(yyjson_arr_size(collections_array)) != st::status_t::success_k ||
            files.reserve(yyjson_arr_size(files_array)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to read the manifest"};

        std::size_t index = 0, count = 0;
        yyjson_val* object = nullptr;
        yyjson_arr_foreach(collections_array, index, count, object) {
            manifest_collection_t collection;
            if (!read_(yyjson_obj_get(object, "id"), collection.id) ||
                !read_(yyjson_obj_get(object, "name"), collection.name) ||
                !read_(yyjson_obj_get(object, "store"), collection.store) ||
                !read_(yyjson_obj_get(object, "value_size"), collection.value_size) ||
                !read_(yyjson_obj_get(object, "value_max_size"), collection.value_max_size) ||
                !read_(yyjson_obj_get(object, "level_enlarge_factor"), collection.level_enlarge_factor) ||
                !read_(yyjson_obj_get(object, "cache_limit"), collection.cache_limit) ||
                !read_(yyjson_obj_get(object, "memory_runs_max_bytes"), collection.memory_runs_max_bytes) ||
                !read_(yyjson_obj_get(object, "memory_runs_max_deltas"), collection.memory_runs_max_deltas))
                return malformed_();
            [[maybe_unused]] st::status_t const placed = collections.push_back(st::assume_reserved,
                                                                               std::move(collection));
        }

        std::size_t paths = 0, offsets = 0;
        yyjson_arr_foreach(files_array, index, count, object) {
            manifest_file_t file;
            yyjson_val* const path = yyjson_obj_get(object, "path");
            yyjson_val* const medium = yyjson_obj_get(object, "medium");
            yyjson_val* const collection = yyjson_obj_get(object, "collection");
            if (path) {
                file.run = run_of(text_of_(path));
                if (!file.run.is_valid()) return {status_code_t::malformed_k, "A manifest path names no run file"};
                ++paths;
            }
            else if (read_(yyjson_obj_get(object, "offset"), file.offset)) {
                yyjson_val* const run = yyjson_obj_get(object, "run");
                if (run && !read_(run, file.run)) return malformed_();
                ++offsets;
            }
            else return malformed_();
            if ((medium && !read_(medium, file.medium)) || !read_(yyjson_obj_get(object, "bytes"), file.bytes))
                return malformed_();
            if (collection) {
                if (!read_(collection, file.collection) || file.collection >= collections.size() ||
                    !read_(yyjson_obj_get(object, "level"), file.level) ||
                    !read_(yyjson_obj_get(object, "stamp"), file.stamp))
                    return malformed_();
            }
            if (yyjson_val* const digest = yyjson_obj_get(object, "sha256"); digest && !read_(digest, file.sha256))
                return malformed_();
            [[maybe_unused]] st::status_t const placed = files.push_back(st::assume_reserved, std::move(file));
        }
        if (paths != 0 && offsets != 0) return {status_code_t::malformed_k, "A manifest mixes paths and offsets"};
        placement = offsets != 0 ? manifest_placement_t::offsets_k : manifest_placement_t::paths_k;
        return {};
    }

    static status_t malformed_() noexcept {
        return {status_code_t::malformed_k, "The manifest lacks a field or holds one out of range"};
    }

    static std::string_view text_of_(yyjson_val* value) noexcept {
        return yyjson_is_str(value) ? std::string_view {yyjson_get_str(value), yyjson_get_len(value)}
                                    : std::string_view {};
    }

    template <std::unsigned_integral value_type_>
    static bool read_(yyjson_val* value, value_type_& number) noexcept {
        if (!yyjson_is_uint(value) || yyjson_get_uint(value) > (std::numeric_limits<value_type_>::max)()) return false;
        number = static_cast<value_type_>(yyjson_get_uint(value));
        return true;
    }

    static bool read_(yyjson_val* value, string_t& text) noexcept {
        if (!yyjson_is_str(value)) return false;
        return sz::succeeded(text.assign(string_view_t {yyjson_get_str(value), yyjson_get_len(value)}));
    }

    static bool read_(yyjson_val* value, uuid128_t& identifier) noexcept {
        std::string_view const text = text_of_(value);
        if (text.size() != uuid128_t::printed_length_k) return false;
        std::span<char const, uuid128_t::printed_length_k> const printed {text.data(), uuid128_t::printed_length_k};
        if (!uuid128_t::parses(printed)) return false;
        identifier = uuid128_t::parse(printed);
        return true;
    }

    static bool read_(yyjson_val* value, sha256_t& digest) noexcept {
        std::string_view const text = text_of_(value);
        if (text.size() != sizeof(digest.bytes) * 2) return false;
        auto const nibble = [](char digit) noexcept -> int {
            if (digit >= '0' && digit <= '9') return digit - '0';
            if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
            return -1;
        };
        for (std::size_t byte = 0; byte != sizeof(digest.bytes); ++byte) {
            int const high = nibble(text[byte * 2]), low = nibble(text[byte * 2 + 1]);
            if (high < 0 || low < 0) return false;
            digest.bytes[byte] = static_cast<std::uint8_t>(high << 4 | low);
        }
        return !digest.empty();
    }

    /** Room for one file name on the stack, which a database name past it refuses. */
    static constexpr std::size_t path_name_capacity_k = 256;
};

#pragma endregion Manifest

} // namespace unum::ustore::utilities
