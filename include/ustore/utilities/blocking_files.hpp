/**
 *  @file include/ustore/utilities/blocking_files.hpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief Whole-file mappings, paths joined into one buffer, directories created and removed, and
 *      shared libraries opened by name, all blocking the calling thread.
 */

#pragma once

#include <cerrno>  // `errno`, `EEXIST`, `ENOENT`, `ENOTDIR`
#include <cstddef> // `std::size_t`
#include <cstring> // `std::memcpy`, `std::strlen`

#if defined(_WIN32)
#include <windows.h> // `CreateFileW`, `MapViewOfFile`, `GetFileAttributesW`, `CreateDirectoryA`, `DeleteFileA`
#else
#include <dlfcn.h>    // `dlopen`, `dlsym`
#include <fcntl.h>    // `open`
#include <sys/mman.h> // `mmap`, `madvise`, `munmap`
#include <sys/stat.h> // `stat`, `fstat`, `S_ISDIR`, `mkdir`
#include <unistd.h>   // `close`, `rmdir`, `unlink`
#endif

#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::exchange`, `std::swap`

#include "ustore/utilities/types.hpp" // `status_t`, `expected`

namespace unum::ustore::utilities {

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

/** Bytes a path of this engine may span, which bounds the one buffer a mount joins paths in. */
inline constexpr std::size_t path_max_bytes_k = 4096;

/** Writes the directory, a separator and the name into @p into, or refuses a path the buffer
 *  will not hold. */
inline status_t join_path(char const* directory, char const* name, std::span<char> into) noexcept {
    std::size_t const directory_bytes = std::strlen(directory);
    std::size_t const name_bytes = std::strlen(name);
    if (directory_bytes + name_bytes + 2 > into.size())
        return {status_code_t::invalid_argument_k, "A path of the database outgrew the platform's limit"};

    std::memcpy(into.data(), directory, directory_bytes);
    std::size_t written = directory_bytes;
    if (written != 0 && into[written - 1] != '/' && into[written - 1] != '\\') into[written++] = '/';
    std::memcpy(into.data() + written, name, name_bytes + 1);
    return {};
}

#pragma endregion Paths

#pragma region Files

/** An owned file descriptor, opened for reading, which a mapping is made from. */
class seekable_file_t {
  public:
#if defined(_WIN32)

    /** What the platform calls an open file. */
    using registered_medium_t = HANDLE;

    /** The value a closed file carries. */
    inline static registered_medium_t const closed_k = INVALID_HANDLE_VALUE;
#else

    /** What the platform calls an open file. */
    using registered_medium_t = int;

    /** The value a closed file carries. */
    inline static constexpr registered_medium_t closed_k = -1;
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

    /** Opens the existing null-terminated @p path for reading. */
    static expected<seekable_file_t> open(char const* path) noexcept;

    /** Byte length of the file. */
    expected<std::size_t> length() const noexcept;

    /** Closes the file, reporting the failure the destructor ignores. */
    status_t close() noexcept;

  private:
    friend class mapped_file_t;

    /** The wrapped descriptor, or @c closed_k after a move or a close. */
    registered_medium_t descriptor_ = closed_k;
};

#if defined(_WIN32)

inline expected<seekable_file_t> seekable_file_t::open(char const* path) noexcept {
    wide_path_t const wide {new (std::nothrow) wchar_t[path_capacity_k]};
    if (!wide) return status_t {status_code_t::out_of_memory_k, "Failed to allocate the wide path"};
    if (!widen_path(path, wide)) return status_t {status_code_t::invalid_argument_k, "The path is not valid UTF-8"};
    seekable_file_t file;
    file.descriptor_ = ::CreateFileW(wide.get(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!file) return status_t {status_code_t::io_k, "Failed to open the file"};
    return file;
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

inline expected<seekable_file_t> seekable_file_t::open(char const* path) noexcept {
    seekable_file_t file;
    file.descriptor_ = ::open(path, O_RDONLY | O_CLOEXEC);
    if (!file) return status_t {status_code_t::io_k, "Failed to open the file"};
    return file;
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

/** A shared library opened by name and kept open for the process's life, as the entry points resolved from it are never
 *  unloaded; empty where the system has no such library. */
struct shared_library_t {
    void* handle = nullptr;

    /** Opens the library named @p path, or answers the empty one. */
    static shared_library_t open(char const* path) noexcept {
#if defined(_WIN32)
        return {reinterpret_cast<void*>(::LoadLibraryA(path))};
#else
        return {::dlopen(path, RTLD_NOW | RTLD_LOCAL)};
#endif
    }

    /** Points @p entry at the symbol exported as @p name, answering whether there is one. */
    template <typename function_type_>
    bool resolve(char const* name, function_type_& entry) const noexcept {
        if (!handle) return false;
#if defined(_WIN32)
        entry = reinterpret_cast<function_type_>(::GetProcAddress(static_cast<HMODULE>(handle), name));
#else
        entry = reinterpret_cast<function_type_>(::dlsym(handle, name));
#endif
        return entry != nullptr;
    }

    explicit operator bool() const noexcept { return handle != nullptr; }
};

#pragma endregion Files

#pragma region Mappings

/** A read-only mapping of a whole file, unmapped on destruction, for parsers that scan large inputs in place. */
class mapped_file_t {
  public:
    mapped_file_t() = default;
    mapped_file_t(mapped_file_t&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)) {}
    mapped_file_t& operator=(mapped_file_t&& other) noexcept {
        std::swap(data_, other.data_);
        std::swap(size_, other.size_);
        return *this;
    }
    mapped_file_t(mapped_file_t const&) = delete;
    mapped_file_t& operator=(mapped_file_t const&) = delete;
    ~mapped_file_t() noexcept;

    /** Maps the null-terminated @p path whole, where an empty file maps to an empty view. */
    static expected<mapped_file_t> map(char const* path) noexcept;

    /** The mapped bytes. */
    std::string_view view() const noexcept { return {data_, size_}; }

  private:

    /** The first mapped byte, or null for an empty file and after a move. */
    char const* data_ = nullptr;
    std::size_t size_ = 0;
};

#if defined(_WIN32)

inline mapped_file_t::~mapped_file_t() noexcept {
    if (data_) { [[maybe_unused]] BOOL const unmapped = ::UnmapViewOfFile(data_); }
}

inline expected<mapped_file_t> mapped_file_t::map(char const* path) noexcept {
    auto [file, open_status] = seekable_file_t::open(path);
    if (open_status.failed()) return open_status;
    auto [length, measure_status] = file.length();
    if (measure_status.failed()) return measure_status;
    mapped_file_t mapped;
    if (length == 0) return mapped;
    // A view keeps its mapping alive, and the mapping its file, so both handles close on return
    HANDLE const mapping = ::CreateFileMappingW(file.descriptor_, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) return status_t {status_code_t::io_k, "Failed to map the file"};
    void const* const begin = ::MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    [[maybe_unused]] BOOL const closed = ::CloseHandle(mapping);
    if (!begin) return status_t {status_code_t::io_k, "Failed to map the file"};
    mapped.data_ = static_cast<char const*>(begin);
    mapped.size_ = length;
    return mapped;
}

#else

inline mapped_file_t::~mapped_file_t() noexcept {
    if (data_) { [[maybe_unused]] int const unmapped = ::munmap(const_cast<char*>(data_), size_); }
}

inline expected<mapped_file_t> mapped_file_t::map(char const* path) noexcept {
    // nvcc 13 crashes on structured bindings of a move-only file here, which GPU compaction's units include
    expected<seekable_file_t> file = seekable_file_t::open(path);
    if (!file) return file.status();
    expected<std::size_t> length = file->length();
    if (!length) return length.status();
    mapped_file_t mapped;
    if (*length == 0) return mapped;
    // A mapping outlives the descriptor it came from, which closes on return
    void* const begin = ::mmap(nullptr, *length, PROT_READ, MAP_PRIVATE, file->descriptor_, 0);
    if (begin == MAP_FAILED) return status_t {status_code_t::io_k, "Failed to map the file"};
    [[maybe_unused]] int const advised = ::madvise(begin, *length, MADV_SEQUENTIAL);
    mapped.data_ = static_cast<char const*>(begin);
    mapped.size_ = *length;
    return mapped;
}

#endif

#pragma endregion Mappings

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

#else

/** Whether the null-terminated @p path names a directory, where a missing path is simply not one. */
inline expected<bool> directory_exists(char const* path) noexcept {
    struct stat information;
    if (::stat(path, &information) == 0) return S_ISDIR(information.st_mode) != 0;
    if (errno == ENOENT || errno == ENOTDIR) return false;
    return status_t {status_code_t::io_k, "Failed to inspect the path"};
}

#endif

#if defined(_WIN32)

/** Creates @p path alone, treating a directory that is already there as success. */
inline status_t create_directory(char const* path) noexcept {
    if (CreateDirectoryA(path, nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS) return {};
    return {status_code_t::io_k, "A directory of the database would not be created"};
}

/** Removes the file @p path, treating one that is already gone as success. */
inline status_t remove_file(char const* path) noexcept {
    if (DeleteFileA(path) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND) return {};
    return {status_code_t::io_k, "A file of the database would not be removed"};
}

/** Removes the empty directory @p path, treating one that is already gone as success. */
inline status_t remove_directory(char const* path) noexcept {
    if (RemoveDirectoryA(path) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND) return {};
    return {status_code_t::io_k, "A directory of the database would not be removed"};
}

#else

/** Creates @p path alone, treating a directory that is already there as success. */
inline status_t create_directory(char const* path) noexcept {
    if (::mkdir(path, 0755) == 0 || errno == EEXIST) return {};
    return {status_code_t::io_k, "A directory of the database would not be created"};
}

/** Removes the file @p path, treating one that is already gone as success. */
inline status_t remove_file(char const* path) noexcept {
    if (::unlink(path) == 0 || errno == ENOENT) return {};
    return {status_code_t::io_k, "A file of the database would not be removed"};
}

/** Removes the empty directory @p path, treating one that is already gone as success. */
inline status_t remove_directory(char const* path) noexcept {
    if (::rmdir(path) == 0 || errno == ENOENT) return {};
    return {status_code_t::io_k, "A directory of the database would not be removed"};
}

#endif

/**
 *  @brief Creates @p path and every parent it needs, which a mount does before it opens
 *      a volume there.
 *
 *  @note Only the last component has to be created; an intermediate that refuses, such as a Windows
 *      drive letter, is passed over rather than reported.
 */
inline status_t create_directories(char const* path) noexcept {
    char components[path_max_bytes_k];
    std::size_t const length = std::strlen(path);
    if (length == 0 || length >= sizeof(components))
        return {status_code_t::invalid_argument_k, "A directory path is one byte to the platform's limit"};
    std::memcpy(components, path, length + 1);

    for (std::size_t index = 1; index != length; ++index) {
        if (components[index] != '/' && components[index] != '\\') continue;
        components[index] = '\0';
        [[maybe_unused]] status_t const parent = create_directory(components);
        components[index] = path[index];
    }
    return create_directory(components);
}

#pragma endregion Directories

} // namespace unum::ustore::utilities
