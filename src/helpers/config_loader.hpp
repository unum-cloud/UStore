/**
 *  @file src/helpers/config_loader.hpp
 *  @author Mesrop Gevorgyan
 *  @date March 9, 2023
 *  @brief Database configuration, parsed and validated from JSON through yyjson.
 */

#pragma once

#include <cstddef>    // `std::size_t`
#include <cstdint>    // `std::int64_t`, `std::uint64_t`, `std::uint8_t`
#include <sys/stat.h> // `stat`, `S_ISDIR`

#include <charconv>     // `std::from_chars`
#include <concepts>     // `std::floating_point`, `std::integral`, `std::same_as`
#include <limits>       // `std::numeric_limits`
#include <string_view>  // `std::string_view`
#include <system_error> // `std::errc`
#include <utility>      // `std::exchange`, `std::in_range`, `std::swap`

#include <yyjson.h>

#include <smashtable/basic_vector.hpp>

#include <ustore/status.hpp> // `status_t`

namespace unum::ustore {

#pragma region Documents

/** Owns a parsed yyjson document and frees it on destruction. */
class json_document_t {
    yyjson_doc* document_ = nullptr;

  public:
    json_document_t() noexcept = default;
    explicit json_document_t(yyjson_doc* document) noexcept : document_(document) {}

    json_document_t(json_document_t const&) = delete;
    json_document_t& operator=(json_document_t const&) = delete;

    json_document_t(json_document_t&& other) noexcept : document_(std::exchange(other.document_, nullptr)) {}

    json_document_t& operator=(json_document_t&& other) noexcept {
        std::swap(document_, other.document_);
        return *this;
    }

    ~json_document_t() noexcept {
        if (document_) yyjson_doc_free(document_);
    }

    /** The root value, or null when nothing is parsed. */
    yyjson_val* root() const noexcept { return document_ ? yyjson_doc_get_root(document_) : nullptr; }
};

/** Parses JSON text that may carry comments into @p document. */
inline status_t parse_json(std::string_view text, json_document_t& document) noexcept {
    yyjson_doc* const parsed = yyjson_read(text.data(), text.size(), YYJSON_READ_ALLOW_COMMENTS);
    if (!parsed) return "Invalid JSON";
    document = json_document_t(parsed);
    return {};
}

/** Parses the JSON file at the null-terminated @p path into @p document. */
inline status_t load_json_file(char const* path, json_document_t& document) noexcept {
    yyjson_doc* const parsed = yyjson_read_file(path, YYJSON_READ_ALLOW_COMMENTS, nullptr, nullptr);
    if (!parsed) return "Config file not found or invalid";
    document = json_document_t(parsed);
    return {};
}

#pragma endregion Documents

#pragma region Members

/** Reads an integer member of @p object, answering false only when it is present but does not fit. */
template <typename integral_type_>
    requires(std::integral<integral_type_> && !std::same_as<integral_type_, bool>)
bool read_json_member(yyjson_val* object, char const* name, integral_type_& number) noexcept {
    yyjson_val* const member = yyjson_obj_get(object, name);
    if (!member) return true;
    if (yyjson_is_uint(member) && std::in_range<integral_type_>(yyjson_get_uint(member))) {
        number = static_cast<integral_type_>(yyjson_get_uint(member));
        return true;
    }
    if (yyjson_is_sint(member) && std::in_range<integral_type_>(yyjson_get_sint(member))) {
        number = static_cast<integral_type_>(yyjson_get_sint(member));
        return true;
    }
    return false;
}

/** Reads a numeric member of @p object, answering false only when it is present but not a number. */
template <typename floating_type_>
    requires std::floating_point<floating_type_>
bool read_json_member(yyjson_val* object, char const* name, floating_type_& number) noexcept {
    yyjson_val* const member = yyjson_obj_get(object, name);
    if (!member) return true;
    if (yyjson_is_real(member)) number = static_cast<floating_type_>(yyjson_get_real(member));
    else if (yyjson_is_uint(member)) number = static_cast<floating_type_>(yyjson_get_uint(member));
    else if (yyjson_is_sint(member)) number = static_cast<floating_type_>(yyjson_get_sint(member));
    else return false;
    return true;
}

/** Reads a boolean member of @p object, answering false only when it is present but not a boolean. */
inline bool read_json_member(yyjson_val* object, char const* name, bool& flag) noexcept {
    yyjson_val* const member = yyjson_obj_get(object, name);
    if (!member) return true;
    if (!yyjson_is_bool(member)) return false;
    flag = yyjson_get_bool(member);
    return true;
}

/** Reads a string member of @p object as a null-terminated view into its document. */
inline bool read_json_member(yyjson_val* object, char const* name, std::string_view& text) noexcept {
    yyjson_val* const member = yyjson_obj_get(object, name);
    if (!member) return true;
    if (!yyjson_is_str(member)) return false;
    text = std::string_view(yyjson_get_str(member), yyjson_get_len(member));
    return true;
}

#pragma endregion Members

#pragma region Parsing

/** Parses a volume such as "512", "512 B" or "1.5 GB" into bytes; only the scaled units take a fraction. */
inline bool parse_bytes(std::string_view text, std::size_t& bytes) noexcept {
    if (text.empty()) {
        bytes = 0;
        return true;
    }
    if (text.front() == '.') return false;

    double number = 0;
    char const* const text_end = text.data() + text.size();
    auto const [number_end, error] = std::from_chars(text.data(), text_end, number);
    if (error != std::errc {}) return false;

    std::string_view const digits(text.data(), static_cast<std::size_t>(number_end - text.data()));
    std::string_view unit(number_end, static_cast<std::size_t>(text_end - number_end));
    while (!unit.empty() && unit.front() == ' ') unit.remove_prefix(1);

    if (unit == "KB") number *= 1024.0;
    else if (unit == "MB") number *= 1024.0 * 1024.0;
    else if (unit == "GB") number *= 1024.0 * 1024.0 * 1024.0;
    else if (unit == "TB") number *= 1024.0 * 1024.0 * 1024.0 * 1024.0;
    else if ((!unit.empty() && unit != "B") || digits.find('.') != std::string_view::npos) return false;

    if (!(number >= 0) || number >= static_cast<double>(std::numeric_limits<std::size_t>::max())) return false;
    bytes = static_cast<std::size_t>(number);
    return true;
}

/** Reads a volume member given either as a byte count or as a string with a unit. */
inline bool read_json_volume(yyjson_val* object, char const* name, std::size_t& bytes) noexcept {
    yyjson_val* const member = yyjson_obj_get(object, name);
    if (yyjson_is_str(member))
        return parse_bytes(std::string_view(yyjson_get_str(member), yyjson_get_len(member)), bytes);
    return !member || (yyjson_is_uint(member) && read_json_member(object, name, bytes));
}

/** Parses a "major.minor" version whose parts each fit a byte. */
inline bool parse_version(std::string_view text, std::uint8_t& major, std::uint8_t& minor) noexcept {
    char const* const text_end = text.data() + text.size();
    auto const [major_end, major_error] = std::from_chars(text.data(), text_end, major);
    if (major_error != std::errc {} || major_end == text_end || *major_end != '.') return false;
    auto const [minor_end, minor_error] = std::from_chars(major_end + 1, text_end, minor);
    return minor_error == std::errc {} && minor_end == text_end;
}

/** Whether the null-terminated @p path names an existing directory. */
inline bool is_directory(char const* path) noexcept {
    struct stat information;
    return ::stat(path, &information) == 0 && S_ISDIR(information.st_mode);
}

#pragma endregion Parsing

#pragma region Configuration

/** Major version of the configuration format this build reads. */
inline constexpr std::uint8_t config_major_version_k = 1;

/** Minor version of the configuration format this build reads. */
inline constexpr std::uint8_t config_minor_version_k = 0;

/** A storage directory and the space the database may occupy in it. */
struct disk_config_t {

    /** Marks space that only the disk itself limits. */
    static constexpr std::size_t unlimited_space_k = std::numeric_limits<std::size_t>::max();

    /** Directory path, null-terminated inside the configuration document. */
    std::string_view path = "";

    /** Space limit in bytes. */
    std::size_t max_size = unlimited_space_k;
};

/** Where the engine reads its own options from. */
struct engine_config_t {

    /** Remote location of the options, which no engine supports. */
    std::string_view config_url = "";

    /** Local options file, null-terminated inside the configuration document. */
    std::string_view config_file_path = "";

    /** Non-empty options object nested in the configuration, or null. */
    yyjson_val* config = nullptr;
};

/** Database configuration, whose strings and nested options point into its own document. */
struct config_t {

    /** Parsed configuration every other member points into. */
    json_document_t document;

    /** Main directory for metadata and data, null-terminated. */
    std::string_view directory = "";

    /** Additional storage directories. */
    st::basic_vector<disk_config_t> data_directories;

    /** Engine-specific options. */
    engine_config_t engine;
};

/** Parses and validates a configuration, filling @p config only as far as it is valid. */
inline status_t load_config(std::string_view text, config_t& config) noexcept {
    if (status_t parsed = parse_json(text, config.document); !parsed) return parsed;

    yyjson_val* const root = config.document.root();
    if (!yyjson_is_obj(root)) return "Config isn't a JSON object";

    // A configuration without a version reads as the current format.
    if (yyjson_obj_get(root, "version")) {
        std::string_view version;
        std::uint8_t major = 0;
        std::uint8_t minor = 0;
        if (!read_json_member(root, "version", version) || !parse_version(version, major, minor))
            return "Invalid version format";
        if (major != config_major_version_k || minor != config_minor_version_k) return "Version not supported";
    }

    if (!read_json_member(root, "directory", config.directory)) return "Invalid directory";

    yyjson_val* const directories = yyjson_obj_get(root, "data_directories");
    if (directories && !yyjson_is_arr(directories)) return "Invalid data directories config";
    if (st::failed(config.data_directories.reserve(yyjson_arr_size(directories))))
        return "Out of memory for data directories";

    yyjson_arr_iter directories_iterator;
    yyjson_arr_iter_init(directories, &directories_iterator);
    while (yyjson_val* const entry = yyjson_arr_iter_next(&directories_iterator)) {
        disk_config_t disk;
        if (!read_json_member(entry, "path", disk.path) || disk.path.empty()) return "Empty data directory path";
        if (!read_json_volume(entry, "max_size", disk.max_size)) return "Invalid volume format";
        if (st::failed(config.data_directories.push_back(std::move(disk)))) return "Out of memory for data directories";
    }

    yyjson_val* const engine = yyjson_obj_get(root, "engine");
    if (!engine) return {};
    if (!yyjson_is_obj(engine) || !read_json_member(engine, "config_url", config.engine.config_url) ||
        !read_json_member(engine, "config_file_path", config.engine.config_file_path))
        return "Invalid engine config";

    yyjson_val* const nested = yyjson_obj_get(engine, "config");
    if (!nested || yyjson_is_null(nested)) return {};
    if (!yyjson_is_obj(nested)) return "Invalid engine config";
    if (yyjson_obj_size(nested) != 0) config.engine.config = nested;
    return {};
}

#pragma endregion Configuration

} // namespace unum::ustore
