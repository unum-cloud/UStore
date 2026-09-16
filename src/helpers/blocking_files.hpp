/**
 *  @file src/helpers/blocking_files.hpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief Positional file reads and writes, and directory queries, that block the calling thread.
 */

#pragma once

#include <cerrno>  // `errno`, `EINTR`, `ENOENT`, `ENOTDIR`
#include <cstddef> // `std::size_t`

#if defined(_WIN32)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h> // `CreateFileW`, `ReadFile`, `WriteFile`, `FindFirstFileW`
#else
#include <dirent.h>   // `opendir`, `readdir`, `closedir`
#include <fcntl.h>    // `open`
#include <sys/stat.h> // `stat`, `fstat`, `S_ISDIR`
#include <unistd.h>   // `pread`, `pwrite`, `close`
#endif

#include <concepts>    // `std::invocable`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::exchange`, `std::swap`

#include <ustore/status.hpp> // `status_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

namespace unum::ustore {

#pragma region Paths

#if defined(_WIN32)

/** Longest path the wide Windows calls accept, including the null terminator. */
inline constexpr std::size_t path_capacity_k = 32768;

/** A heap buffer for one wide path, since the longest one does not fit a thread's stack comfortably. */
using wide_path_t = std::unique_ptr<wchar_t[]>;

/** Converts the null-terminated UTF-8 @p path into @p wide, answering how many wide characters it took with the
 *  terminator, or zero when it did not fit or decode. */
inline int widen_path(char const* path, wide_path_t const& wide, std::size_t reserved = 0) noexcept {
    return ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide.get(),
                                 static_cast<int>(path_capacity_k - reserved));
}

#endif

#pragma endregion Paths

#pragma region Files

/** How @c seekable_file_t::open opens a file: reading an existing one, or creating a fresh one. */
enum class file_mode_t { read_k, write_k };

/** An owned file descriptor with positional reads and writes only, so any thread addresses any offset. */
class seekable_file_t {
  public:
#if defined(_WIN32)

    /** What the platform calls an open file. */
    using descriptor_t = HANDLE;

    /** The value a closed file carries. */
    inline static descriptor_t const closed_k = INVALID_HANDLE_VALUE;
#else

    /** What the platform calls an open file. */
    using descriptor_t = int;

    /** The value a closed file carries. */
    inline static constexpr descriptor_t closed_k = -1;
#endif

    seekable_file_t() = default;
    seekable_file_t(seekable_file_t&& other) noexcept : descriptor_(std::exchange(other.descriptor_, closed_k)) {}
    seekable_file_t& operator=(seekable_file_t&& other) noexcept {
        std::swap(descriptor_, other.descriptor_);
        return *this;
    }
    seekable_file_t(seekable_file_t const&) = delete;
    seekable_file_t& operator=(seekable_file_t const&) = delete;
    ~seekable_file_t() noexcept { [[maybe_unused]] status_t const closed = close(); }

    /** Whether a file is open. */
    explicit operator bool() const noexcept { return descriptor_ != closed_k; }

    /** Opens the null-terminated @p path under @p mode. */
    static expected<seekable_file_t> open(char const* path, file_mode_t mode) noexcept;

    /** Writes every byte of @p source at @p offset, looping over short writes. */
    status_t write(std::size_t offset, std::span<byte_t const> source) noexcept;

    /** Reads exactly @p destination.size() bytes at @p offset, where a short read is a failure. */
    status_t read(std::size_t offset, std::span<byte_t> destination) const noexcept;

    /** Byte length of the file. */
    expected<std::size_t> length() const noexcept;

    /** Closes the file, reporting the failure the destructor ignores. */
    status_t close() noexcept;

  private:

    /** The wrapped descriptor, or @c closed_k after a move or a close. */
    descriptor_t descriptor_ = closed_k;
};

#if defined(_WIN32)

inline expected<seekable_file_t> seekable_file_t::open(char const* path, file_mode_t mode) noexcept {
    wide_path_t const wide {new (std::nothrow) wchar_t[path_capacity_k]};
    if (!wide) return status_t {status_code_t::out_of_memory_k, "Failed to allocate the wide path"};
    if (!widen_path(path, wide)) return status_t {status_code_t::invalid_argument_k, "The path is not valid UTF-8"};
    seekable_file_t file;
    file.descriptor_ = mode == file_mode_t::read_k ? ::CreateFileW(wide.get(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)
                                                   : ::CreateFileW(wide.get(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!file) return status_t {status_code_t::io_k, "Failed to open the file"};
    return file;
}

inline status_t seekable_file_t::write(std::size_t offset, std::span<byte_t const> source) noexcept {
    while (!source.empty()) {
        OVERLAPPED overlapped {};
        overlapped.Offset = static_cast<DWORD>(offset);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD const asked = static_cast<DWORD>(source.size() < 0x7FFFFFFFu ? source.size() : 0x7FFFFFFFu);
        DWORD written = 0;
        if (!::WriteFile(descriptor_, source.data(), asked, &written, &overlapped) || !written)
            return {status_code_t::io_k, "Failed to write the file"};
        source = source.subspan(written);
        offset += written;
    }
    return {};
}

inline status_t seekable_file_t::read(std::size_t offset, std::span<byte_t> destination) const noexcept {
    while (!destination.empty()) {
        OVERLAPPED overlapped {};
        overlapped.Offset = static_cast<DWORD>(offset);
        overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD const asked = static_cast<DWORD>(destination.size() < 0x7FFFFFFFu ? destination.size() : 0x7FFFFFFFu);
        DWORD received = 0;
        if (!::ReadFile(descriptor_, destination.data(), asked, &received, &overlapped) || !received)
            return {status_code_t::io_k, "Failed to read the file"};
        destination = destination.subspan(received);
        offset += received;
    }
    return {};
}

inline expected<std::size_t> seekable_file_t::length() const noexcept {
    LARGE_INTEGER size;
    if (!::GetFileSizeEx(descriptor_, &size)) return status_t {status_code_t::io_k, "Failed to measure the file"};
    return static_cast<std::size_t>(size.QuadPart);
}

inline status_t seekable_file_t::close() noexcept {
    if (descriptor_ == closed_k) return {};
    if (!::CloseHandle(std::exchange(descriptor_, closed_k))) return {status_code_t::io_k, "Failed to close the file"};
    return {};
}

#else

inline expected<seekable_file_t> seekable_file_t::open(char const* path, file_mode_t mode) noexcept {
    int const flags = mode == file_mode_t::read_k ? O_RDONLY : (O_RDWR | O_CREAT | O_TRUNC);
    seekable_file_t file;
    file.descriptor_ = ::open(path, flags | O_CLOEXEC, 0644);
    if (!file) return status_t {status_code_t::io_k, "Failed to open the file"};
    return file;
}

inline status_t seekable_file_t::write(std::size_t offset, std::span<byte_t const> source) noexcept {
    while (!source.empty()) {
        ssize_t const written = ::pwrite(descriptor_, source.data(), source.size(), static_cast<off_t>(offset));
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return {status_code_t::io_k, "Failed to write the file"};
        source = source.subspan(static_cast<std::size_t>(written));
        offset += static_cast<std::size_t>(written);
    }
    return {};
}

inline status_t seekable_file_t::read(std::size_t offset, std::span<byte_t> destination) const noexcept {
    while (!destination.empty()) {
        ssize_t const received = ::pread(descriptor_, destination.data(), destination.size(),
                                         static_cast<off_t>(offset));
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) return {status_code_t::io_k, "Failed to read the file"};
        destination = destination.subspan(static_cast<std::size_t>(received));
        offset += static_cast<std::size_t>(received);
    }
    return {};
}

inline expected<std::size_t> seekable_file_t::length() const noexcept {
    struct stat description;
    if (::fstat(descriptor_, &description) != 0) return status_t {status_code_t::io_k, "Failed to measure the file"};
    return static_cast<std::size_t>(description.st_size);
}

inline status_t seekable_file_t::close() noexcept {
    if (descriptor_ == closed_k) return {};
    if (::close(std::exchange(descriptor_, closed_k)) != 0) return {status_code_t::io_k, "Failed to close the file"};
    return {};
}

#endif

#pragma endregion Files

#pragma region Directories

#if defined(_WIN32)

/** Whether the null-terminated @p path names a directory, where a missing path is simply not one. */
inline expected<bool> directory_exists(char const* path) noexcept {
    wide_path_t const wide {new (std::nothrow) wchar_t[path_capacity_k]};
    if (!wide) return status_t {status_code_t::out_of_memory_k, "Failed to allocate the wide path"};
    if (!widen_path(path, wide)) return status_t {status_code_t::invalid_argument_k, "The path is not valid UTF-8"};
    DWORD const attributes = ::GetFileAttributesW(wide.get());
    if (attributes != INVALID_FILE_ATTRIBUTES) return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    DWORD const failure = ::GetLastError();
    if (failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND) return false;
    return status_t {status_code_t::io_k, "Failed to inspect the path"};
}

/** Calls @p callback with the UTF-8 name of every entry of the null-terminated @p directory but "." and "..". */
template <typename callback_type_>
    requires std::invocable<callback_type_&, std::string_view>
status_t for_each_directory_entry(char const* directory, callback_type_&& callback) noexcept {
    wide_path_t const pattern {new (std::nothrow) wchar_t[path_capacity_k]};
    if (!pattern) return {status_code_t::out_of_memory_k, "Failed to allocate the wide path"};
    int const widened = widen_path(directory, pattern, 2);
    if (!widened) return {status_code_t::invalid_argument_k, "The path is not valid UTF-8"};
    std::size_t const terminator = static_cast<std::size_t>(widened) - 1;
    pattern[terminator] = L'\\';
    pattern[terminator + 1] = L'*';
    pattern[terminator + 2] = L'\0';

    WIN32_FIND_DATAW entry;
    HANDLE const search = ::FindFirstFileW(pattern.get(), &entry);
    if (search == INVALID_HANDLE_VALUE) return {status_code_t::io_k, "Failed to list the directory"};

    char name[MAX_PATH * 4];
    status_t error;
    do {
        int const length = ::WideCharToMultiByte(CP_UTF8, 0, entry.cFileName, -1, name, sizeof(name), nullptr, nullptr);
        if (length <= 0) {
            error = {status_code_t::malformed_k, "A directory entry is not valid UTF-16"};
            break;
        }
        std::string_view const view {name, static_cast<std::size_t>(length - 1)};
        if (view != "." && view != "..") callback(view);
    } while (::FindNextFileW(search, &entry));
    if (error.succeeded() && ::GetLastError() != ERROR_NO_MORE_FILES)
        error = {status_code_t::io_k, "Failed to list the directory"};
    ::FindClose(search);
    return error;
}

#else

/** Whether the null-terminated @p path names a directory, where a missing path is simply not one. */
inline expected<bool> directory_exists(char const* path) noexcept {
    struct stat information;
    if (::stat(path, &information) == 0) return S_ISDIR(information.st_mode) != 0;
    if (errno == ENOENT || errno == ENOTDIR) return false;
    return status_t {status_code_t::io_k, "Failed to inspect the path"};
}

/** Calls @p callback with the name of every entry of the null-terminated @p directory but "." and "..". */
template <typename callback_type_>
    requires std::invocable<callback_type_&, std::string_view>
status_t for_each_directory_entry(char const* directory, callback_type_&& callback) noexcept {
    DIR* const listing = ::opendir(directory);
    if (!listing) return {status_code_t::io_k, "Failed to list the directory"};
    errno = 0;
    while (dirent const* const entry = ::readdir(listing)) {
        std::string_view const name {entry->d_name};
        if (name != "." && name != "..") callback(name);
        errno = 0;
    }
    bool const listed = errno == 0;
    ::closedir(listing);
    if (!listed) return {status_code_t::io_k, "Failed to list the directory"};
    return {};
}

#endif

#pragma endregion Directories

} // namespace unum::ustore
