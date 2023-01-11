/**
 * @file file.hpp
 * @author Ashot Vardanian
 *
 * @brief Reading/writing from/to disk.
 */
#pragma once
#include <cstdio> // `std::FILE`

#include "ukv/cpp/status.hpp" // `status_t`

namespace unum::ukv {

class file_handle_t {
    std::FILE* handle_ = nullptr;

  public:
    status_t open(char const* path, char const* mode) {
        if (handle_)
            return "Close previous file before opening the new one!";
        handle_ = std::fopen(path, mode);
        if (!handle_)
            return "Failed to open a file";
        return {};
    }

    status_t close() {
        if (!handle_)
            return {};
        if (std::fclose(handle_) == EOF)
            return "Couldn't close the file after write.";
        else
            handle_ = nullptr;
        return {};
    }

    ~file_handle_t() {
        if (handle_)
            std::fclose(handle_);
    }

    operator std::FILE*() const noexcept { return handle_; }
};

} // namespace unum::ukv
