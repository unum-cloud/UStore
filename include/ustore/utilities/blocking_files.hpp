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
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::strlen`, `std::strrchr`

#include "ustore/types.h" // `USTORE_OS_WINDOWS_`

#if USTORE_OS_WINDOWS_
#include <windows.h> // `CreateFileW`, `MapViewOfFile`, `GetFileAttributesW`, `CreateDirectoryA`, `DeleteFileA`
#else
#include <dlfcn.h>       // `dlopen`, `dlsym`
#include <dirent.h>      // `opendir`, `readdir`, `closedir`
#include <fcntl.h>       // `open`
#include <sys/file.h>    // `flock`
#include <sys/mman.h>    // `mmap`, `madvise`, `munmap`
#include <sys/stat.h>    // `stat`, `fstat`, `S_ISDIR`, `mkdir`
#include <sys/statvfs.h> // `statvfs`
#include <unistd.h>      // `close`, `rmdir`, `unlink`, `write`, `read`, `fdatasync`, `fsync`
#endif

#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::exchange`, `std::swap`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/types.hpp" // `status_t`, `expected`

namespace unum::ustore::utilities {

#pragma region Paths

#if USTORE_OS_WINDOWS_

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
#if USTORE_OS_WINDOWS_

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

#if USTORE_OS_WINDOWS_

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
#if USTORE_OS_WINDOWS_
        return {reinterpret_cast<void*>(::LoadLibraryA(path))};
#else
        return {::dlopen(path, RTLD_NOW | RTLD_LOCAL)};
#endif
    }

    /** Points @p entry at the symbol exported as @p name, answering whether there is one. */
    template <typename function_type_>
    bool resolve(char const* name, function_type_& entry) const noexcept {
        if (!handle) return false;
#if USTORE_OS_WINDOWS_
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

#if USTORE_OS_WINDOWS_

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

#if USTORE_OS_WINDOWS_

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

#if USTORE_OS_WINDOWS_

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

#pragma region Durable Files

#if USTORE_OS_WINDOWS_

/** Replaces @p path with @p bytes so a crash leaves the old file or the new one whole: a sibling
 *  @c .tmp is written and flushed, then moved over @p path with write-through. */
inline status_t replace_file_durably(char const* path, std::span<char const> bytes) noexcept {
    char temporary[path_max_bytes_k];
    std::size_t const length = std::strlen(path);
    if (length + 5 > sizeof(temporary)) return {status_code_t::invalid_argument_k, "A path outgrew the limit"};
    std::memcpy(temporary, path, length);
    std::memcpy(temporary + length, ".tmp", 5);
    HANDLE const file = ::CreateFileA(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
    if (file == INVALID_HANDLE_VALUE) return {status_code_t::io_k, "A manifest copy would not be created"};
    DWORD written = 0;
    BOOL const wrote = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    BOOL const flushed = wrote && written == bytes.size() && ::FlushFileBuffers(file);
    [[maybe_unused]] BOOL const closed = ::CloseHandle(file);
    if (!flushed) return {status_code_t::io_k, "A manifest copy would not be written"};
    if (!::MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return {status_code_t::io_k, "A manifest copy would not replace the manifest"};
    return {};
}

/** Reads the whole of @p path into @p into, answering @c not_found_k for a missing file. */
inline status_t read_file(char const* path, st::vector<char>& into) noexcept {
    HANDLE const file = ::CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return ::GetLastError() == ERROR_FILE_NOT_FOUND ? status_t {status_code_t::not_found_k, "No such file"}
                                                        : status_t {status_code_t::io_k, "A file would not open"};
    LARGE_INTEGER size {};
    status_t result;
    DWORD read = 0;
    if (!::GetFileSizeEx(file, &size)) result = {status_code_t::io_k, "A file would not be measured"};
    else if (into.resize(static_cast<std::size_t>(size.QuadPart)) != st::status_t::success_k)
        result = {status_code_t::out_of_memory_k, "No room to read a file"};
    else if (!::ReadFile(file, into.data(), static_cast<DWORD>(into.size()), &read, nullptr) || read != into.size())
        result = {status_code_t::io_k, "A file would not be read"};
    [[maybe_unused]] BOOL const closed = ::CloseHandle(file);
    return result;
}

/** Calls @p visitor with the name of every entry of the directory @p path. */
template <typename visitor_type_>
status_t list_directory(char const* path, visitor_type_&& visitor) noexcept {
    char pattern[path_max_bytes_k];
    if (status_t const joined = join_path(path, "*", pattern); joined.failed()) return joined;
    WIN32_FIND_DATAA found {};
    HANDLE const search = ::FindFirstFileA(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return {};
    do visitor(std::string_view {found.cFileName});
    while (::FindNextFileA(search, &found));
    [[maybe_unused]] BOOL const closed = ::FindClose(search);
    return {};
}

/** What @c link_or_copy_file did to land a file at its target. */
enum class file_landing_t : unsigned char {
    linked_k,
    copied_k,
};

/** Hard-links @p source at @p target, or copies it whole and flushes the copy where no link
 *  reaches across volumes. */
inline expected<file_landing_t> link_or_copy_file(char const* source, char const* target) noexcept {
    if (::CreateHardLinkA(target, source, nullptr)) return file_landing_t::linked_k;
    if (!::CopyFileA(source, target, TRUE)) return status_t {status_code_t::io_k, "A run file would not be copied"};
    return file_landing_t::copied_k;
}

/** Bytes the filesystem holding @p directory still offers an unprivileged writer. */
inline expected<std::uint64_t> free_bytes_of(char const* directory) noexcept {
    ULARGE_INTEGER available {};
    if (!::GetDiskFreeSpaceExA(directory, &available, nullptr, nullptr))
        return status_t {status_code_t::io_k, "A data directory would not report its free space"};
    return static_cast<std::uint64_t>(available.QuadPart);
}

#else

/** Replaces @p path with @p bytes so a crash leaves the old file or the new one whole: a
 *  sibling @c .tmp is written and @c fdatasync -ed, renamed over @p path, and the directory is
 *  @c fsync -ed. */
inline status_t replace_file_durably(char const* path, std::span<char const> bytes) noexcept {
    char temporary[path_max_bytes_k];
    std::size_t const length = std::strlen(path);
    if (length + 5 > sizeof(temporary)) return {status_code_t::invalid_argument_k, "A path outgrew the limit"};
    std::memcpy(temporary, path, length);
    std::memcpy(temporary + length, ".tmp", 5);
    int const file = ::open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (file < 0) return {status_code_t::io_k, "A manifest copy would not be created"};
    std::size_t written = 0;
    while (written != bytes.size()) {
        ssize_t const step = ::write(file, bytes.data() + written, bytes.size() - written);
        if (step <= 0 && errno != EINTR) break;
        if (step > 0) written += static_cast<std::size_t>(step);
    }
    bool const synced = written == bytes.size() && ::fdatasync(file) == 0;
    [[maybe_unused]] int const closed = ::close(file);
    if (!synced) return {status_code_t::io_k, "A manifest copy would not be written"};
    if (::rename(temporary, path) != 0) return {status_code_t::io_k, "A manifest copy would not replace the manifest"};

    char directory[path_max_bytes_k];
    std::memcpy(directory, path, length + 1);
    char* const slash = std::strrchr(directory, '/');
    if (slash == directory) slash[1] = '\0';
    else if (slash) *slash = '\0';
    else std::memcpy(directory, ".", 2);
    int const parent = ::open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) return {status_code_t::io_k, "The manifest's directory would not open"};
    bool const durable = ::fsync(parent) == 0;
    [[maybe_unused]] int const closed_parent = ::close(parent);
    return durable ? status_t {} : status_t {status_code_t::io_k, "The manifest's directory would not sync"};
}

/** Reads the whole of @p path into @p into, answering @c not_found_k for a missing file. */
inline status_t read_file(char const* path, st::vector<char>& into) noexcept {
    int const file = ::open(path, O_RDONLY | O_CLOEXEC);
    if (file < 0)
        return errno == ENOENT ? status_t {status_code_t::not_found_k, "No such file"}
                               : status_t {status_code_t::io_k, "A file would not open"};
    struct stat information;
    status_t result;
    if (::fstat(file, &information) != 0) result = {status_code_t::io_k, "A file would not be measured"};
    else if (into.resize(static_cast<std::size_t>(information.st_size)) != st::status_t::success_k)
        result = {status_code_t::out_of_memory_k, "No room to read a file"};
    for (std::size_t read = 0; result.succeeded() && read != into.size();) {
        ssize_t const step = ::read(file, into.data() + read, into.size() - read);
        if (step > 0) read += static_cast<std::size_t>(step);
        else if (step == 0 || errno != EINTR) result = {status_code_t::io_k, "A file would not be read"};
    }
    [[maybe_unused]] int const closed = ::close(file);
    return result;
}

/** Calls @p visitor with the name of every entry of the directory @p path. */
template <typename visitor_type_>
status_t list_directory(char const* path, visitor_type_&& visitor) noexcept {
    DIR* const directory = ::opendir(path);
    if (!directory) return {status_code_t::io_k, "A data directory would not be listed"};
    while (dirent const* const entry = ::readdir(directory)) visitor(std::string_view {entry->d_name});
    [[maybe_unused]] int const closed = ::closedir(directory);
    return {};
}

/** What @c link_or_copy_file did to land a file at its target. */
enum class file_landing_t : unsigned char {
    linked_k,
    copied_k,
};

/** Hard-links @p source at @p target, or copies it whole and @c fdatasync -s the copy where no link
 *  reaches across filesystems. */
inline expected<file_landing_t> link_or_copy_file(char const* source, char const* target) noexcept {
    if (::link(source, target) == 0) return file_landing_t::linked_k;
    if (errno != EXDEV && errno != EPERM) return status_t {status_code_t::io_k, "A run file would not be linked"};
    int const from = ::open(source, O_RDONLY | O_CLOEXEC);
    if (from < 0) return status_t {status_code_t::io_k, "A run file would not open to be copied"};
    int const into = ::open(target, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (into < 0) {
        [[maybe_unused]] int const closed = ::close(from);
        return status_t {status_code_t::io_k, "A run file copy would not be created"};
    }
    char buffer[64 * 1024];
    bool copied = true;
    for (;;) {
        ssize_t const read = ::read(from, buffer, sizeof(buffer));
        if (read == 0) break;
        if (read < 0) {
            if (errno == EINTR) continue;
            copied = false;
            break;
        }
        for (ssize_t written = 0; copied && written != read;) {
            ssize_t const step = ::write(into, buffer + written, static_cast<std::size_t>(read - written));
            if (step > 0) written += step;
            else if (step < 0 && errno != EINTR) copied = false;
        }
        if (!copied) break;
    }
    copied = copied && ::fdatasync(into) == 0;
    [[maybe_unused]] int const closed_from = ::close(from);
    [[maybe_unused]] int const closed_into = ::close(into);
    if (!copied) return status_t {status_code_t::io_k, "A run file would not be copied"};
    return file_landing_t::copied_k;
}

/** Bytes the filesystem holding @p directory still offers an unprivileged writer. */
inline expected<std::uint64_t> free_bytes_of(char const* directory) noexcept {
    struct statvfs information;
    if (::statvfs(directory, &information) != 0)
        return status_t {status_code_t::io_k, "A data directory would not report its free space"};
    return static_cast<std::uint64_t>(information.f_bavail) * information.f_frsize;
}

#endif

#pragma endregion Durable Files

#pragma region File Locks

/** An exclusive advisory lock on a file or a device, held until it dies, which serializes the
 *  processes claiming slots of one raw medium. */
class file_lock_t {
  public:
#if USTORE_OS_WINDOWS_
    using native_t = HANDLE;
    inline static native_t const unlocked_k = INVALID_HANDLE_VALUE;
#else
    using native_t = int;
    inline static constexpr native_t unlocked_k = -1;
#endif

    file_lock_t() noexcept = default;
    file_lock_t(file_lock_t&& other) noexcept : held_(std::exchange(other.held_, unlocked_k)) {}
    file_lock_t& operator=(file_lock_t&& other) noexcept {
        std::swap(held_, other.held_);
        return *this;
    }
    file_lock_t(file_lock_t const&) = delete;
    file_lock_t& operator=(file_lock_t const&) = delete;
    ~file_lock_t() noexcept { release_(); }

    /** Locks @p path without waiting, answering @c contended_k while another holder keeps it. */
    static expected<file_lock_t> acquire(char const* path) noexcept;

  private:
    void release_() noexcept;

    native_t held_ = unlocked_k;
};

#if USTORE_OS_WINDOWS_

inline expected<file_lock_t> file_lock_t::acquire(char const* path) noexcept {
    file_lock_t lock;
    lock.held_ = ::CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lock.held_ == unlocked_k) return status_t {status_code_t::io_k, "A raw medium would not open to be locked"};
    OVERLAPPED whole {};
    if (!::LockFileEx(lock.held_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &whole))
        return status_t {status_code_t::contended_k, "Another process claims slots of that raw medium"};
    return lock;
}

inline void file_lock_t::release_() noexcept {
    if (held_ == unlocked_k) return;
    [[maybe_unused]] BOOL const closed = ::CloseHandle(std::exchange(held_, unlocked_k));
}

#else

inline expected<file_lock_t> file_lock_t::acquire(char const* path) noexcept {
    file_lock_t lock;
    lock.held_ = ::open(path, O_RDONLY | O_CLOEXEC);
    if (lock.held_ < 0) return status_t {status_code_t::io_k, "A raw medium would not open to be locked"};
    if (::flock(lock.held_, LOCK_EX | LOCK_NB) != 0)
        return errno == EWOULDBLOCK
                   ? status_t {status_code_t::contended_k, "Another process claims slots of that raw medium"}
                   : status_t {status_code_t::io_k, "A raw medium would not be locked"};
    return lock;
}

inline void file_lock_t::release_() noexcept {
    if (held_ == unlocked_k) return;
    [[maybe_unused]] int const closed = ::close(std::exchange(held_, unlocked_k));
}

#endif

#pragma endregion File Locks

} // namespace unum::ustore::utilities
