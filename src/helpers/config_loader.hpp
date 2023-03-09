/**
 * @file config_loader.hpp
 * @author Ashot Vardanian
 *
 * @brief DBMS configurations
 */
#pragma once

#include <string>            // `std::string`
#include <nlohmann/json.hpp> // `nlohmann::json`

#include "ukv/cpp/status.hpp" // `status_t`

namespace unum::ukv {

/**
 * @brief Storage disk configuration
 *
 * @path: Data directory path on the disk
 * @max_size: Space limit used by DBMS
 */
struct disk_config_t {
    constexpr size_t unlimited_space_k = std::numeric_limit<size_t>::max(); // Not limited by software

    std::string path;
    size_t max_size = unlimited_space_k;
};

/**
 * @brief DBMS configuration
 *
 * @directory: Main path where DB stores metadata, e.g schema, log, etc.
 * @data_directories: Storage paths where DB stores data.
 * @engine_config_path: Engine specific config file path.
 */
struct config_t {
    std::string directory;
    darray_gt<disk_config_t> data_directories;
    std::string engine_config_path;
};

/**
 * @brief Loads DBMS configuration from json
 */
class config_loader_t {
  public:
    using json_t = nlohmann::json;

  public:
    static constexpr u8_t current_major_version_k = 1;
    static constexpr u8_t current_minor_version_k = 0;

  public:
    static inline status_t load(json_t const& json, config_t& config) noexcept;

  private:
    static inline status_t validate_config(json_t const& json);

    static inline bool_t parse_version(std::string const& str_version, u8_t& major, u8_t& minor) noexcept;
    static inline bool_t parse_volume(json_t const& json, string_t const& key, size_t& bytes) noexcept;
    static inline bool_t parse_bytes(string_t const& str, size_t& bytes) noexcept;
};

inline status_t config_loader_t::load(json_t const& json, config_t& config) noexcept {

    try {
        auto status = validate_config(json);
        if (!status)
            return status;

        config.directory = json.value("directory");
        config.engine_config_path = json.value("engine_config_path");

        if (json.contains("data_directories")) {
            auto j_disks = json["data_directories"];
            if (j_disks.is_array()) {
                for (auto j_disk : j_disks) {
                    disk_config_t disk;
                    disk.path = j_disk.value("path");
                    disk.max_size = j_disk.value("max_size");
                    if (disk.path.empty())
                        return "Empty data directory path";
                    if (!parse_volume(j_disk, "max_size", disk.max_size))
                        return "Invalid volume format";
                    config.data_directories.push_back(std::move(disk));
                }
            }
            else
                return "Invalid data directories config";
        }
    }
    catch (...) {
        return "Exception occurred: Failed to load configs";
    }

    return {};
}

inline status_t config_loader_t::validate_config(json_t const& json) {

    std::string version = json.value("version", std::string());
    u8_t major_version = 0;
    u8_t minor_version = 0;
    if (!parse_version(version.c_str(), major_version, minor_version))
        return "Invalid version format";
    if (major_version != current_major_version_k || minor_version != current_minor_version_k)
        return "Version not supported";
    return {};
}

inline bool_t config_loader_t::parse_version(std::string const& str_version, u8_t& major, u8_t& minor) noexcept {

    int mj = 0;
    int mn = 0;
    try {
        size_t pos = 0;
        std::string str = str_version.c_str();
        mj = std::stoul(str, &pos);
        if (pos == str.size())
            return false;
        str = str.substr(++pos);
        mn = std::stoul(str, &pos);
        if (pos < str.size())
            return false;
    }
    catch (...) {
        return false;
    }
    if (mj < 0 || mn < 0 || mj > int(std::numeric_limits<u8_t>::max()) || mn > int(std::numeric_limits<u8_t>::max()))
        return false;

    major = static_cast<u8_t>(mj);
    minor = static_cast<u8_t>(mn);
    return true;
}

inline bool_t config_loader_t::parse_volume(json_t const& json, string_t const& key, size_t& bytes) noexcept {

    auto it = json.find(key.c_str());
    if (it == json.end())
        return true; // Skip if not exist

    switch (it->type()) {
    case json_t::value_t::number_unsigned: {
        bytes = it.value();
        return true;
    }
    case json_t::value_t::string: {
        string_t value = std::string(it.value()).c_str();
        return parse_bytes(value, bytes);
    }
    default: break;
    }

    return false;
}

inline bool_t config_loader_t::parse_bytes(string_t const& str, size_t& bytes) noexcept {

    std::stringstream ss(str.c_str());

    // Parse number
    double number = 0.0;
    if (str.view().starts_with(".") || (ss >> number).fail() || std::isnan(number))
        return false;

    // Parse unite
    std::string metric;
    if (ss >> metric) {
        if (metric == "KB")
            number *= 1024ull;
        else if (metric == "MB")
            number *= 1024 * 1024ull;
        else if (metric == "GB")
            number *= 1024 * 1024 * 1024ull;
        else if (metric == "TB")
            number *= 1024 * 1024 * 1024 * 1024ull;
        else if (metric != "B" || str.find('.') != size_max_k)
            return false;
    }
    else if (str.find('.') != size_max_k)
        return false;

    if (!ss.eof())
        return false;
    if (std::isnan(number) || number > std::numeric_limits<size_t>::max())
        return false;

    bytes = static_cast<size_t>(number);
    return true;
}

} // namespace unum::ukv
