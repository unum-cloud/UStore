/**
 *  @file src/helpers/file.hpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief File handles and directory queries that report failures as statuses instead of throwing.
 */

#pragma once

#include <cstdio> // `std::FILE`, `std::fopen`, `std::fclose`

#include <concepts>     // `std::invocable`
#include <filesystem>   // `std::filesystem::status`, `std::filesystem::directory_iterator`
#include <system_error> // `std::error_code`
#include <utility>      // `std::exchange`

#include <ustore/status.hpp> // `status_t`, `expected`

namespace unum::ustore {

/** An open C stream that closes itself. */
class file_handle_t {
    std::FILE* handle_ = nullptr;

  public:
    file_handle_t() = default;
    file_handle_t(file_handle_t const&) = delete;
    file_handle_t& operator=(file_handle_t const&) = delete;

    ~file_handle_t() noexcept {
        if (!handle_) return;
        [[maybe_unused]] int const closed = std::fclose(handle_);
    }

    status_t open(char const* path, char const* mode) noexcept {
        if (handle_) return status_t::status_view("Close previous file before opening the new one!");
        handle_ = std::fopen(path, mode);
        if (!handle_) return status_t::status_view("Failed to open a file");
        return {};
    }

    status_t close() noexcept {
        if (!handle_) return {};
        if (std::fclose(std::exchange(handle_, nullptr)) == EOF)
            return status_t::status_view("Couldn't close the file after write.");
        return {};
    }

    operator std::FILE*() const noexcept { return handle_; }
};

/** Whether @p path names a directory, where a missing path is simply not one. */
inline expected<bool> directory_exists(std::filesystem::path const& path) noexcept {
    std::error_code error;
    std::filesystem::file_status const status = std::filesystem::status(path, error);
    if (error && status.type() != std::filesystem::file_type::not_found)
        return status_t::status_view("Failed to inspect the path");
    return std::filesystem::is_directory(status);
}

/**
 *  Calls @p callback on every entry of @p directory and stops at the first failure to read the listing.
 *  @param[in] directory Directory to list.
 *  @param[in] callback Invoked with each @c std::filesystem::directory_entry in turn.
 */
template <typename callback_>
    requires std::invocable<callback_&, std::filesystem::directory_entry const&>
status_t for_each_directory_entry(std::filesystem::path const& directory, callback_&& callback) noexcept {
    std::error_code error;
    std::filesystem::directory_iterator entry {directory, error};
    for (; !error && entry != std::filesystem::directory_iterator {}; entry.increment(error)) callback(*entry);
    if (error) return status_t::status_view("Failed to list the directory");
    return {};
}

} // namespace unum::ustore
