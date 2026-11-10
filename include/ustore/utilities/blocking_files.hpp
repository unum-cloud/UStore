/**
 *  @file include/ustore/utilities/blocking_files.hpp
 *  @author Ash Vardanian
 *  @date January 11, 2023
 *  @brief Native files read and written at absolute offsets, whole-file mappings, paths joined
 *      into one buffer, directories created and removed, files replaced durably, and shared
 *      libraries opened by name, all blocking the calling thread.
 */

#pragma once

#include <cerrno>  // `errno`, `EEXIST`, `ENOENT`, `ENOTDIR`, `ENOSPC`, `EDQUOT`, `EXDEV`, `EPERM`, `ENOTSUP`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstdio>  // `std::rename`
#include <cstring> // `std::memcpy`, `std::strlen`

#include "ustore/types.h" // `USTORE_OS_WINDOWS_`, `USTORE_OS_APPLE_`

#if USTORE_OS_WINDOWS_
#include <windows.h> // `CreateFileW`, `ReadFile`, `WriteFile`, `MapViewOfFile`, `MoveFileExW`, `CreateHardLinkW`
#else
#include <dlfcn.h>       // `dlopen`, `dlsym`
#include <dirent.h>      // `opendir`, `readdir`, `closedir`
#include <fcntl.h>       // `open`, `fcntl`, `O_DIRECT`, `F_NOCACHE`, `F_FULLFSYNC`
#include <sys/file.h>    // `flock`
#include <sys/mman.h>    // `mmap`, `madvise`, `munmap`
#include <sys/stat.h>    // `stat`, `fstat`, `S_ISDIR`, `S_ISBLK`, `mkdir`
#include <sys/statvfs.h> // `statvfs`
#include <unistd.h>      // `close`, `rmdir`, `unlink`, `link`, `pread`, `pwrite`, `ftruncate`, `fdatasync`, `fsync`
#endif

#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::exchange`, `std::swap`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/utilities/types.hpp" // `status_t`, `expected`, `byte_t`

namespace unum::ustore::utilities {

#pragma region Paths

#if USTORE_OS_WINDOWS_

/** Longest path the wide Windows calls accept, including the null terminator. */
inline constexpr std::size_t path_capacity_k = 32768;

/** A heap buffer for one wide path, since the longest one does not fit a thread's stack comfortably. */
using wide_path_t = std::unique_ptr<wchar_t[]>;

/** Converts the null-terminated UTF-8 @p path into a wide one, refusing one that will not fit. */
inline expected<wide_path_t> widen_path(char const* path) noexcept {
    wide_path_t wide {new (std::nothrow) wchar_t[path_capacity_k]};
    if (!wide) return status_t {status_code_t::out_of_memory_k, "Failed to allocate the wide path"};
    if (!::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide.get(), static_cast<int>(path_capacity_k)))
        return status_t {status_code_t::invalid_argument_k, "The path is not valid UTF-8"};
    return wide;
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

/** Writes into @p into the sibling a durable replacement of @p path stages its bytes in: the same
 *  name with @c .tmp appended. */
inline status_t temporary_path_of(char const* path, std::span<char> into) noexcept {
    constexpr std::string_view suffix = ".tmp";
    std::size_t const length = std::strlen(path);
    if (length + suffix.size() + 1 > into.size())
        return {status_code_t::invalid_argument_k, "A path outgrew the limit"};
    std::memcpy(into.data(), path, length);
    std::memcpy(into.data() + length, suffix.data(), suffix.size());
    into[length + suffix.size()] = '\0';
    return {};
}

/** Writes the directory holding @p path into @p into: the root for a file at the root, and @c .
 *  for a bare name. */
inline status_t parent_directory_of(char const* path, std::span<char> into) noexcept {
    std::size_t const length = std::strlen(path);
    if (length + 2 > into.size()) return {status_code_t::invalid_argument_k, "A path outgrew the limit"};
    std::size_t separator = length;
    while (separator != 0 && path[separator - 1] != '/' && path[separator - 1] != '\\') --separator;
    std::size_t const kept = separator == 0 ? 0 : separator == 1 ? 1 : separator - 1;
    if (kept == 0) {
        into[0] = '.';
        into[1] = '\0';
        return {};
    }
    std::memcpy(into.data(), path, kept);
    into[kept] = '\0';
    return {};
}

#pragma endregion Paths

#pragma region Native Files

/** What a caller may do to a file, and whether opening it creates or empties it. */
enum class file_access_t : std::uint8_t {

    /** Reads alone, of a file that must already exist. */
    read_only_k,

    /** Reads and writes, of a file that must already exist. */
    read_write_k,

    /** Reads and writes, creating the file when absent and keeping what it holds otherwise. */
    create_read_write_k,

    /** Reads and writes, of a file created or emptied by the open. */
    truncate_read_write_k,
};

/** Whether the page cache stands between the caller and the medium. */
enum class file_buffering_t : std::uint8_t {

    /** The kernel caches, which costs a copy and accepts any alignment. */
    buffered_k,

    /** The kernel does not cache, which needs aligned memory, offsets and lengths. */
    unbuffered_k,
};

/** The largest run one positioned call carries, kept well inside every platform's own limit. */
inline constexpr std::uint64_t native_transfer_bytes_k = 1024ull * 1024ull * 1024ull;

#if USTORE_OS_WINDOWS_

/** The handle one open file is addressed by, opened overlapped so no call shares a cursor. */
using native_file_t = HANDLE;

/** The handle value that marks a file that never opened. */
inline native_file_t const invalid_native_file_k = INVALID_HANDLE_VALUE;

/** Translates the platform's failure into the status the callers report. */
inline status_t native_file_failure(DWORD reason, char const* message) noexcept {
    switch (reason) {
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: return {status_code_t::capacity_exhausted_k, message};
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return {status_code_t::not_found_k, message};
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return {status_code_t::already_exists_k, message};
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_HANDLE: return {status_code_t::invalid_argument_k, message};
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY: return {status_code_t::out_of_memory_k, message};
    default: return {status_code_t::io_k, message};
    }
}

/** The manual-reset event one thread waits on for its own positioned transfers, so threads sharing
 *  a handle never wait on each other's completions. */
class native_file_event_t {
  public:
    native_file_event_t() noexcept = default;
    native_file_event_t(native_file_event_t&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    native_file_event_t& operator=(native_file_event_t&& other) noexcept {
        std::swap(handle_, other.handle_);
        return *this;
    }
    native_file_event_t(native_file_event_t const&) = delete;
    native_file_event_t& operator=(native_file_event_t const&) = delete;
    ~native_file_event_t() noexcept {
        if (handle_) { [[maybe_unused]] BOOL const closed = ::CloseHandle(handle_); }
    }

    /** Creates the event, or reports why the platform refused. */
    static expected<native_file_event_t> make() noexcept {
        native_file_event_t event;
        event.handle_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event.handle_) return native_file_failure(::GetLastError(), "An event would not be created");
        return event;
    }

    /** The handle an @c OVERLAPPED transfer signals. */
    HANDLE handle() const noexcept { return handle_; }

  private:
    HANDLE handle_ = nullptr;
};

#else

/** The descriptor one open file is addressed by. */
using native_file_t = int;

/** The descriptor value that marks a file that never opened. */
inline constexpr native_file_t invalid_native_file_k = -1;

/** Translates the platform's failure into the status the callers report. */
inline status_t native_file_failure(int reason, char const* message) noexcept {
    switch (reason) {
    case ENOSPC:
    case EDQUOT: return {status_code_t::capacity_exhausted_k, message};
    case ENOENT: return {status_code_t::not_found_k, message};
    case EEXIST: return {status_code_t::already_exists_k, message};
    case EINVAL:
    case EBADF: return {status_code_t::invalid_argument_k, message};
    case ENOMEM: return {status_code_t::out_of_memory_k, message};
    default: return {status_code_t::io_k, message};
    }
}

/** Nothing, since positioned calls here block the calling thread and need nothing to wait on. */
struct native_file_event_t {

    /** Makes the empty event, which never fails. */
    static expected<native_file_event_t> make() noexcept { return native_file_event_t {}; }
};

/** The open flags of @p access, closed on @c exec. */
inline int native_open_flags_of(file_access_t access) noexcept {
    switch (access) {
    case file_access_t::read_only_k: return O_RDONLY | O_CLOEXEC;
    case file_access_t::read_write_k: return O_RDWR | O_CLOEXEC;
    case file_access_t::create_read_write_k: return O_RDWR | O_CREAT | O_CLOEXEC;
    case file_access_t::truncate_read_write_k: return O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC;
    }
    return O_RDONLY | O_CLOEXEC;
}

#endif

#if USTORE_OS_WINDOWS_

/** Opens @p path for overlapped positioned calls, or reports why the platform refused. */
inline expected<native_file_t> open_native_file(char const* path, file_access_t access,
                                                file_buffering_t buffering) noexcept {
    auto [wide, widen_status] = widen_path(path);
    if (widen_status.failed()) return widen_status;
    DWORD const rights = access == file_access_t::read_only_k ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE;
    DWORD const creation = access == file_access_t::create_read_write_k     ? OPEN_ALWAYS
                           : access == file_access_t::truncate_read_write_k ? CREATE_ALWAYS
                                                                            : OPEN_EXISTING;
    DWORD flags = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED;
    if (buffering == file_buffering_t::unbuffered_k) flags |= FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH;
    native_file_t const file = ::CreateFileW(wide.get(), rights, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, creation,
                                             flags, nullptr);
    if (file == invalid_native_file_k) return native_file_failure(::GetLastError(), "The file would not open");
    return file;
}

/** Makes every landed write durable. */
inline status_t flush_native_file(native_file_t file) noexcept {
    if (!::FlushFileBuffers(file)) return native_file_failure(::GetLastError(), "The file would not flush");
    return {};
}

#elif USTORE_OS_APPLE_

/** Opens @p path, or reports why the platform refused. */
inline expected<native_file_t> open_native_file(char const* path, file_access_t access,
                                                file_buffering_t buffering) noexcept {
    native_file_t const file = ::open(path, native_open_flags_of(access), 0644);
    if (file == invalid_native_file_k) return native_file_failure(errno, "The file would not open");
    // A hint rather than O_DIRECT: unaligned reads fall back to the cache, and cached pages serve
    if (buffering == file_buffering_t::unbuffered_k) {
        [[maybe_unused]] int const hinted = ::fcntl(file, F_NOCACHE, 1);
    }
    return file;
}

/** Makes every landed write durable past the drive's cache, or as far as @c fsync reaches where
 *  the filesystem has no such barrier. */
inline status_t flush_native_file(native_file_t file) noexcept {
    if (::fcntl(file, F_FULLFSYNC) == 0) return {};
    if (errno == ENOTSUP && ::fsync(file) == 0) return {};
    return native_file_failure(errno, "The file would not flush");
}

#else

/** Opens @p path, or reports why the platform refused. */
inline expected<native_file_t> open_native_file(char const* path, file_access_t access,
                                                file_buffering_t buffering) noexcept {
    int const unbuffered = buffering == file_buffering_t::unbuffered_k ? O_DIRECT : 0;
    native_file_t const file = ::open(path, native_open_flags_of(access) | unbuffered, 0644);
    if (file == invalid_native_file_k) return native_file_failure(errno, "The file would not open");
    return file;
}

/** Makes every landed write durable, without waiting on metadata a read does not need. */
inline status_t flush_native_file(native_file_t file) noexcept {
    if (::fdatasync(file) != 0) return native_file_failure(errno, "The file would not flush");
    return {};
}

#endif

#if USTORE_OS_WINDOWS_

/** Closes @p file, which must have opened. */
inline status_t close_native_file(native_file_t file) noexcept {
    if (!::CloseHandle(file)) return native_file_failure(::GetLastError(), "The file would not close");
    return {};
}

/** Reads at most @p buffer bytes from @p offset, waiting on @p event, answering what arrived. */
inline expected<std::uint64_t> read_native_file(native_file_t file, std::uint64_t offset, std::span<byte_t> buffer,
                                                native_file_event_t const& event) noexcept {
    std::uint64_t moved = 0;
    while (moved != buffer.size()) {
        OVERLAPPED position {};
        std::uint64_t const at = offset + moved;
        position.Offset = static_cast<DWORD>(at & 0xFFFFFFFFull);
        position.OffsetHigh = static_cast<DWORD>(at >> 32);
        position.hEvent = event.handle();
        std::uint64_t const remaining = buffer.size() - moved;
        DWORD const asking = static_cast<DWORD>(remaining < native_transfer_bytes_k ? remaining
                                                                                    : native_transfer_bytes_k);
        DWORD arrived = 0;
        DWORD const started = ::ReadFile(file, buffer.data() + moved, asking, nullptr, &position) ? ERROR_SUCCESS
                                                                                                  : ::GetLastError();
        if (started == ERROR_HANDLE_EOF) break;
        if (started != ERROR_SUCCESS && started != ERROR_IO_PENDING)
            return native_file_failure(started, "A positioned read failed");
        if (!::GetOverlappedResult(file, &position, &arrived, TRUE)) {
            DWORD const finished = ::GetLastError();
            if (finished == ERROR_HANDLE_EOF) break;
            return native_file_failure(finished, "A positioned read failed");
        }
        if (arrived == 0) break;
        moved += arrived;
    }
    return moved;
}

/** Writes at most @p buffer bytes at @p offset, waiting on @p event, answering how many landed,
 *  which a full volume cuts short. */
inline expected<std::uint64_t> write_native_file(native_file_t file, std::uint64_t offset,
                                                 std::span<byte_t const> buffer,
                                                 native_file_event_t const& event) noexcept {
    std::uint64_t moved = 0;
    while (moved != buffer.size()) {
        OVERLAPPED position {};
        std::uint64_t const at = offset + moved;
        position.Offset = static_cast<DWORD>(at & 0xFFFFFFFFull);
        position.OffsetHigh = static_cast<DWORD>(at >> 32);
        position.hEvent = event.handle();
        std::uint64_t const remaining = buffer.size() - moved;
        DWORD const asking = static_cast<DWORD>(remaining < native_transfer_bytes_k ? remaining
                                                                                    : native_transfer_bytes_k);
        DWORD landed = 0;
        DWORD const started = ::WriteFile(file, buffer.data() + moved, asking, nullptr, &position) ? ERROR_SUCCESS
                                                                                                   : ::GetLastError();
        DWORD const finished = started != ERROR_SUCCESS && started != ERROR_IO_PENDING ? started
                               : ::GetOverlappedResult(file, &position, &landed, TRUE) ? ERROR_SUCCESS
                                                                                       : ::GetLastError();
        if (finished == ERROR_DISK_FULL || finished == ERROR_HANDLE_DISK_FULL) break;
        if (finished != ERROR_SUCCESS) return native_file_failure(finished, "A positioned write failed");
        if (landed == 0) break;
        moved += landed;
    }
    return moved;
}

/** Resizes the file to @p bytes without moving any cursor, which a mount does once to preallocate
 *  a volume. */
inline status_t truncate_native_file(native_file_t file, std::uint64_t bytes) noexcept {
    FILE_END_OF_FILE_INFO end {};
    end.EndOfFile.QuadPart = static_cast<LONGLONG>(bytes);
    if (!::SetFileInformationByHandle(file, FileEndOfFileInfo, &end, sizeof(end)))
        return native_file_failure(::GetLastError(), "The file would not resize");
    return {};
}

/** The bytes the file currently holds, or zero when the platform would not say. */
inline std::uint64_t native_file_capacity(native_file_t file) noexcept {
    LARGE_INTEGER size {};
    return ::GetFileSizeEx(file, &size) ? static_cast<std::uint64_t>(size.QuadPart) : 0;
}

#else

/** Closes @p file, which must have opened. */
inline status_t close_native_file(native_file_t file) noexcept {
    if (::close(file) != 0) return native_file_failure(errno, "The file would not close");
    return {};
}

/** Reads at most @p buffer bytes from @p offset, answering how many arrived. */
inline expected<std::uint64_t> read_native_file(native_file_t file, std::uint64_t offset, std::span<byte_t> buffer,
                                                native_file_event_t const&) noexcept {
    std::uint64_t moved = 0;
    while (moved != buffer.size()) {
        std::uint64_t const remaining = buffer.size() - moved;
        std::size_t const asking = remaining < native_transfer_bytes_k ? remaining : native_transfer_bytes_k;
        ssize_t const arrived = ::pread(file, buffer.data() + moved, asking, static_cast<off_t>(offset + moved));
        if (arrived < 0) {
            if (errno == EINTR) continue;
            return native_file_failure(errno, "A positioned read failed");
        }
        if (arrived == 0) break;
        moved += static_cast<std::uint64_t>(arrived);
    }
    return moved;
}

/** Writes at most @p buffer bytes at @p offset, answering how many landed, which a full volume
 *  cuts short. */
inline expected<std::uint64_t> write_native_file(native_file_t file, std::uint64_t offset,
                                                 std::span<byte_t const> buffer, native_file_event_t const&) noexcept {
    std::uint64_t moved = 0;
    while (moved != buffer.size()) {
        std::uint64_t const remaining = buffer.size() - moved;
        std::size_t const asking = remaining < native_transfer_bytes_k ? remaining : native_transfer_bytes_k;
        ssize_t const landed = ::pwrite(file, buffer.data() + moved, asking, static_cast<off_t>(offset + moved));
        if (landed < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOSPC || errno == EDQUOT) break;
            return native_file_failure(errno, "A positioned write failed");
        }
        if (landed == 0) break;
        moved += static_cast<std::uint64_t>(landed);
    }
    return moved;
}

/** Resizes a regular file to @p bytes, which a mount does once to preallocate a volume; a block
 *  device is refused. */
inline status_t truncate_native_file(native_file_t file, std::uint64_t bytes) noexcept {
    struct stat description {};
    if (::fstat(file, &description) == 0 && S_ISBLK(description.st_mode))
        return {status_code_t::invalid_argument_k, "A block device keeps the capacity of its medium"};
    if (::ftruncate(file, static_cast<off_t>(bytes)) != 0)
        return native_file_failure(errno, "The file would not resize");
    return {};
}

/** The bytes the file currently holds, or zero when the platform would not say. */
inline std::uint64_t native_file_capacity(native_file_t file) noexcept {
    struct stat description {};
    return ::fstat(file, &description) == 0 ? static_cast<std::uint64_t>(description.st_size) : 0;
}

#endif

#pragma endregion Native Files

#pragma region Files

/** An owned native file, opened for reading, which a mapping is made from. */
class seekable_file_t {
  public:
    seekable_file_t() = default;
    seekable_file_t(seekable_file_t&& other) noexcept : file_(std::exchange(other.file_, invalid_native_file_k)) {}
    seekable_file_t& operator=(seekable_file_t&& other) noexcept {
        std::swap(file_, other.file_);
        return *this;
    }
    seekable_file_t(seekable_file_t const&) = delete;
    seekable_file_t& operator=(seekable_file_t const&) = delete;
    ~seekable_file_t() noexcept { [[maybe_unused]] status_t const closed = close(); }

    /** Whether a file is open. */
    explicit operator bool() const noexcept { return file_ != invalid_native_file_k; }

    /** Opens the existing null-terminated @p path for reading. */
    static expected<seekable_file_t> open(char const* path) noexcept {
        expected<native_file_t> const file = open_native_file(path, file_access_t::read_only_k,
                                                              file_buffering_t::buffered_k);
        if (!file) return file.status();
        seekable_file_t opened;
        opened.file_ = *file;
        return opened;
    }

    /** Byte length of the file. */
    expected<std::size_t> length() const noexcept { return static_cast<std::size_t>(native_file_capacity(file_)); }

    /** Closes the file, reporting the failure the destructor ignores. */
    status_t close() noexcept {
        if (file_ == invalid_native_file_k) return {};
        return close_native_file(std::exchange(file_, invalid_native_file_k));
    }

  private:
    friend class mapped_file_t;

    /** The wrapped file, or @c invalid_native_file_k after a move or a close. */
    native_file_t file_ = invalid_native_file_k;
};

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
    HANDLE const mapping = ::CreateFileMappingW(file.file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
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
    void* const begin = ::mmap(nullptr, *length, PROT_READ, MAP_PRIVATE, file->file_, 0);
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

/** Whether the null-terminated @p path names a directory, where a missing path is not one. */
inline expected<bool> directory_exists(char const* path) noexcept {
    auto [wide, widen_status] = widen_path(path);
    if (widen_status.failed()) return widen_status;
    DWORD const attributes = ::GetFileAttributesW(wide.get());
    if (attributes != INVALID_FILE_ATTRIBUTES) return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    DWORD const failure = ::GetLastError();
    if (failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND) return false;
    return status_t {status_code_t::io_k, "Failed to inspect the path"};
}

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

/** Moves @p source over @p target in one step, replacing a target that exists, and returns once
 *  the move is on the medium. */
inline status_t rename_file(char const* source, char const* target) noexcept {
    auto [wide_source, source_status] = widen_path(source);
    if (source_status.failed()) return source_status;
    auto [wide_target, target_status] = widen_path(target);
    if (target_status.failed()) return target_status;
    if (!::MoveFileExW(wide_source.get(), wide_target.get(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return native_file_failure(::GetLastError(), "A file would not be renamed");
    return {};
}

/** Names the file @p source again as @p target, answering @c unsupported_k where no link reaches
 *  across volumes. */
inline status_t link_file(char const* source, char const* target) noexcept {
    auto [wide_source, source_status] = widen_path(source);
    if (source_status.failed()) return source_status;
    auto [wide_target, target_status] = widen_path(target);
    if (target_status.failed()) return target_status;
    if (::CreateHardLinkW(wide_target.get(), wide_source.get(), nullptr)) return {};
    DWORD const reason = ::GetLastError();
    if (reason == ERROR_NOT_SAME_DEVICE || reason == ERROR_NOT_SUPPORTED || reason == ERROR_INVALID_FUNCTION)
        return {status_code_t::unsupported_k, "No link reaches across these volumes"};
    return native_file_failure(reason, "A file would not be linked");
}

#else

/** Whether the null-terminated @p path names a directory, where a missing path is not one. */
inline expected<bool> directory_exists(char const* path) noexcept {
    struct stat information;
    if (::stat(path, &information) == 0) return S_ISDIR(information.st_mode) != 0;
    if (errno == ENOENT || errno == ENOTDIR) return false;
    return status_t {status_code_t::io_k, "Failed to inspect the path"};
}

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

/** Moves @p source over @p target in one step, replacing a target that exists; the move is
 *  durable once the directory is synced. */
inline status_t rename_file(char const* source, char const* target) noexcept {
    if (std::rename(source, target) != 0) return native_file_failure(errno, "A file would not be renamed");
    return {};
}

/** Names the file @p source again as @p target, answering @c unsupported_k where no link reaches
 *  across filesystems. */
inline status_t link_file(char const* source, char const* target) noexcept {
    if (::link(source, target) == 0) return {};
    if (errno == EXDEV || errno == EPERM)
        return {status_code_t::unsupported_k, "No link reaches across these filesystems"};
    return native_file_failure(errno, "A file would not be linked");
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

/** Makes the entries of the directory @p path durable, flushing it through a handle that only
 *  backup semantics open. */
inline status_t sync_directory(char const* path) noexcept {
    auto [wide, widen_status] = widen_path(path);
    if (widen_status.failed()) return widen_status;
    HANDLE const directory = ::CreateFileW(wide.get(), GENERIC_READ | GENERIC_WRITE,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory == INVALID_HANDLE_VALUE)
        return native_file_failure(::GetLastError(), "A directory would not open to be synced");
    DWORD const flushed = ::FlushFileBuffers(directory) ? ERROR_SUCCESS : ::GetLastError();
    [[maybe_unused]] BOOL const closed = ::CloseHandle(directory);
    if (flushed != ERROR_SUCCESS) return native_file_failure(flushed, "A directory would not sync");
    return {};
}

#elif USTORE_OS_APPLE_

/** Makes the entries of the directory @p path durable past the drive's cache, or as far as
 *  @c fsync reaches where the filesystem has no such barrier. */
inline status_t sync_directory(char const* path) noexcept {
    int const directory = ::open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return native_file_failure(errno, "A directory would not open to be synced");
    int const synced = ::fcntl(directory, F_FULLFSYNC) == 0 || ::fsync(directory) == 0 ? 0 : errno;
    [[maybe_unused]] int const closed = ::close(directory);
    if (synced != 0) return native_file_failure(synced, "A directory would not sync");
    return {};
}

#else

/** Makes the entries of the directory @p path durable, which the filesystem promises only for a
 *  directory that is itself @c fsync -ed. */
inline status_t sync_directory(char const* path) noexcept {
    int const directory = ::open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return native_file_failure(errno, "A directory would not open to be synced");
    int const synced = ::fsync(directory) == 0 ? 0 : errno;
    [[maybe_unused]] int const closed = ::close(directory);
    if (synced != 0) return native_file_failure(synced, "A directory would not sync");
    return {};
}

#endif

/** Streams bytes into a native file from its start, writing them in runs of
 *  @c pending_capacity_k, so a writer of many small fields issues few calls. */
class file_appender_t {
  public:

    /** Bytes held back before one write carries them. */
    static constexpr std::size_t pending_capacity_k = 64 * 1024;

    file_appender_t(native_file_t file, native_file_event_t const& event) noexcept : file_(file), event_(event) {}
    file_appender_t(file_appender_t const&) = delete;
    file_appender_t& operator=(file_appender_t const&) = delete;

    /** Appends @p bytes, writing every full run on the way. */
    status_t append(std::span<byte_t const> bytes) noexcept {
        while (!bytes.empty()) {
            std::size_t const room = pending_capacity_k - pending_bytes_;
            std::size_t const taken = bytes.size() < room ? bytes.size() : room;
            std::memcpy(pending_ + pending_bytes_, bytes.data(), taken);
            pending_bytes_ += taken;
            bytes = bytes.subspan(taken);
            if (pending_bytes_ == pending_capacity_k)
                if (status_t const written = write_tail(); written.failed()) return written;
        }
        return {};
    }

    /** Appends everything @p source holds, read from its start through the pending run. */
    status_t append_file(native_file_t source) noexcept {
        for (std::uint64_t offset = 0;;) {
            std::span<byte_t> const room {pending_ + pending_bytes_, pending_capacity_k - pending_bytes_};
            auto [arrived, read_status] = read_native_file(source, offset, room, event_);
            if (read_status.failed()) return read_status;
            if (arrived == 0) return {};
            offset += arrived;
            pending_bytes_ += static_cast<std::size_t>(arrived);
            if (pending_bytes_ == pending_capacity_k)
                if (status_t const written = write_tail(); written.failed()) return written;
        }
    }

    /** Writes whatever is still pending, which a writer does once after its last append. */
    status_t write_tail() noexcept {
        std::span<byte_t const> const pending {pending_, pending_bytes_};
        auto [landed, write_status] = write_native_file(file_, offset_, pending, event_);
        if (write_status.failed()) return write_status;
        if (landed != pending.size()) return {status_code_t::capacity_exhausted_k, "The volume filled up"};
        offset_ += landed;
        pending_bytes_ = 0;
        return {};
    }

  private:
    native_file_t file_;
    native_file_event_t const& event_;
    std::uint64_t offset_ = 0;
    std::size_t pending_bytes_ = 0;
    byte_t pending_[pending_capacity_k];
};

/**
 *  @brief Replaces @p path with what @p writer appends, so that after a crash either the old file
 *      or the new one is there whole.
 *
 *  The bytes go to a sibling @c .tmp, which is flushed, closed, renamed over @p path, and made
 *  durable by syncing the directory; a failure before the rename removes the sibling.
 *
 *  @param[in] path The file to replace, which need not exist yet.
 *  @param[in] writer Called once with a @c file_appender_t&, answering a status.
 */
template <typename writer_type_>
status_t replace_file_durably(char const* path, writer_type_&& writer) noexcept {
    char temporary[path_max_bytes_k], directory[path_max_bytes_k];
    if (status_t const named = temporary_path_of(path, temporary); named.failed()) return named;
    if (status_t const parent = parent_directory_of(path, directory); parent.failed()) return parent;
    auto [event, event_status] = native_file_event_t::make();
    if (event_status.failed()) return event_status;
    auto [file, open_status] = open_native_file(temporary, file_access_t::truncate_read_write_k,
                                                file_buffering_t::buffered_k);
    if (open_status.failed()) return open_status;
    file_appender_t appender {file, event};
    status_t written = writer(appender);
    if (written.succeeded()) written = appender.write_tail();
    if (written.succeeded()) written = flush_native_file(file);
    status_t const closed = close_native_file(file);
    if (written.succeeded()) written = closed;
    if (written.succeeded()) written = rename_file(temporary, path);
    if (written.failed()) {
        [[maybe_unused]] status_t const removed = remove_file(temporary);
        return written;
    }
    return sync_directory(directory);
}

/** Replaces @p path with @p bytes, so a crash leaves the old file or the new one whole. */
inline status_t replace_file_durably(char const* path, std::span<char const> bytes) noexcept {
    return replace_file_durably(
        path, [bytes](file_appender_t& appender) noexcept { return appender.append(std::as_bytes(bytes)); });
}

/** Reads the whole of @p path into @p into, answering @c not_found_k for a missing file. */
inline status_t read_file(char const* path, st::vector<char>& into) noexcept {
    auto [event, event_status] = native_file_event_t::make();
    if (event_status.failed()) return event_status;
    auto [file, open_status] = open_native_file(path, file_access_t::read_only_k, file_buffering_t::buffered_k);
    if (open_status.failed()) return open_status;
    status_t result;
    if (into.resize(static_cast<std::size_t>(native_file_capacity(file))) != st::status_t::success_k)
        result = {status_code_t::out_of_memory_k, "No room to read a file"};
    else {
        auto [arrived, read_status] = read_native_file(file, 0, std::as_writable_bytes(std::span<char> {into}), event);
        if (read_status.failed()) result = read_status;
        else if (arrived != into.size()) result = {status_code_t::io_k, "A file ended before it was read"};
    }
    [[maybe_unused]] status_t const closed = close_native_file(file);
    return result;
}

#if USTORE_OS_WINDOWS_

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

/** Bytes the filesystem holding @p directory still offers an unprivileged writer. */
inline expected<std::uint64_t> free_bytes_of(char const* directory) noexcept {
    ULARGE_INTEGER available {};
    if (!::GetDiskFreeSpaceExA(directory, &available, nullptr, nullptr))
        return status_t {status_code_t::io_k, "A data directory would not report its free space"};
    return static_cast<std::uint64_t>(available.QuadPart);
}

#else

/** Calls @p visitor with the name of every entry of the directory @p path. */
template <typename visitor_type_>
status_t list_directory(char const* path, visitor_type_&& visitor) noexcept {
    DIR* const directory = ::opendir(path);
    if (!directory) return {status_code_t::io_k, "A data directory would not be listed"};
    while (dirent const* const entry = ::readdir(directory)) visitor(std::string_view {entry->d_name});
    [[maybe_unused]] int const closed = ::closedir(directory);
    return {};
}

/** Bytes the filesystem holding @p directory still offers an unprivileged writer. */
inline expected<std::uint64_t> free_bytes_of(char const* directory) noexcept {
    struct statvfs information;
    if (::statvfs(directory, &information) != 0)
        return status_t {status_code_t::io_k, "A data directory would not report its free space"};
    return static_cast<std::uint64_t>(information.f_bavail) * information.f_frsize;
}

#endif

/** What @c link_or_copy_file did to land a file at its target. */
enum class file_landing_t : unsigned char {
    linked_k,
    copied_k,
};

/** Hard-links @p source at @p target and syncs the directory gaining it, or, where no link reaches
 *  across volumes, replaces @p target durably with a whole copy, so no partial target survives. */
inline expected<file_landing_t> link_or_copy_file(char const* source, char const* target) noexcept {
    status_t const linked = link_file(source, target);
    if (linked.succeeded()) {
        char directory[path_max_bytes_k];
        if (status_t const parent = parent_directory_of(target, directory); parent.failed()) return parent;
        if (status_t const synced = sync_directory(directory); synced.failed()) return synced;
        return file_landing_t::linked_k;
    }
    if (linked != status_code_t::unsupported_k) return linked;
    auto [from, open_status] = open_native_file(source, file_access_t::read_only_k, file_buffering_t::buffered_k);
    if (open_status.failed()) return open_status;
    status_t const copied = replace_file_durably(
        target, [from](file_appender_t& appender) noexcept { return appender.append_file(from); });
    [[maybe_unused]] status_t const closed = close_native_file(from);
    if (copied.failed()) return copied;
    return file_landing_t::copied_k;
}

#pragma endregion Durable Files

#pragma region File Locks

/** An exclusive advisory lock on a file, a directory or a device, held until it dies, which
 *  serializes the mounts claiming slots of one raw medium or the runs of one data directory. */
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
    // Backup semantics open a directory as well as a file.
    lock.held_ = ::CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (lock.held_ == unlocked_k) return status_t {status_code_t::io_k, "A path would not open to be locked"};
    OVERLAPPED whole {};
    if (!::LockFileEx(lock.held_, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &whole))
        return status_t {status_code_t::contended_k, "Another mount holds that medium or directory"};
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
    if (lock.held_ < 0) return status_t {status_code_t::io_k, "A path would not open to be locked"};
    if (::flock(lock.held_, LOCK_EX | LOCK_NB) != 0)
        return errno == EWOULDBLOCK
                   ? status_t {status_code_t::contended_k, "Another mount holds that medium or directory"}
                   : status_t {status_code_t::io_k, "A path would not be locked"};
    return lock;
}

inline void file_lock_t::release_() noexcept {
    if (held_ == unlocked_k) return;
    [[maybe_unused]] int const closed = ::close(std::exchange(held_, unlocked_k));
}

#endif

#pragma endregion File Locks

} // namespace unum::ustore::utilities
