/**
 *  @file src/nvme/blocking_files_disk.hpp
 *  @author Ash Vardanian
 *  @date September 18, 2026
 *  @brief The portable floor of the ring: positioned reads and writes on one file, fanned out over threads.
 *
 *  This is the transport every platform has. POSIX issues @c pread and @c pwrite, Windows issues
 *  @c ReadFile and @c WriteFile with an @c OVERLAPPED offset, and neither keeps a file cursor, so the
 *  same registered medium serves every executor thread at once. Depth comes from those threads rather than
 *  from a device queue, which is why the floor is a floor and not a medium.
 */

#pragma once

#include <cerrno>  // `errno`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#if defined(_WIN32)
#include <windows.h> // `ReadFile`, `WriteFile`, `OVERLAPPED`
#else
#include <fcntl.h>    // `open`, `O_RDONLY`, `O_DIRECT`
#include <sys/stat.h> // `fstat`, `S_ISBLK`
#include <unistd.h>   // `pread`, `pwrite`, `ftruncate`, `close`
#endif
#if defined(__linux__)
#include <linux/fs.h>  // `BLKGETSIZE64`
#include <sys/ioctl.h> // `ioctl`
#endif

#include <span>    // `std::span`
#include <utility> // `std::exchange`, `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/disk.hpp"   // `basic_disk`
#include "nvme/extent.hpp" // `extent_t`, `portable_alignment_k`
#include "nvme/ring.hpp"   // `ring_capabilities_t`

namespace unum::ustore::nvme {

#pragma region Access

/** Whether the page cache stands between the ring and the medium. */
enum class file_buffering_t : std::uint8_t {

    /** The kernel caches, which costs a copy and accepts any alignment. */
    buffered_k,

    /** The kernel does not cache, which needs aligned memory, offsets and lengths. */
    unbuffered_k,
};

#pragma endregion Access

#pragma region Platform

#if defined(_WIN32)

/** The handle one open file is addressed by. */
using native_file_t = void*;

/** The handle value that marks a file that never opened. */
inline native_file_t const invalid_native_file_k = INVALID_HANDLE_VALUE;

/** Opens @p path, or reports why the platform refused. */
inline expected<native_file_t> open_native_file(char const* path, medium_access_t access,
                                                file_buffering_t buffering) noexcept {
    DWORD const rights = access == medium_access_t::read_only_k ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE;
    DWORD const creation = access == medium_access_t::create_read_write_k ? OPEN_ALWAYS : OPEN_EXISTING;
    DWORD flags = FILE_ATTRIBUTE_NORMAL;
    if (buffering == file_buffering_t::unbuffered_k) flags |= FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH;
    native_file_t const file = CreateFileA(path, rights, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, creation, flags,
                                           nullptr);
    if (file == invalid_native_file_k) return status_t {status_code_t::input_output_k, "The file would not open"};
    return file;
}

/** Closes @p file, which must have opened. */
inline void close_native_file(native_file_t file) noexcept { CloseHandle(file); }

/** Reads at most @p buffer bytes from @p offset, answering how many arrived. */
inline expected<std::uint64_t> read_native_file(native_file_t file, std::uint64_t offset,
                                                std::span<byte_t> buffer) noexcept {
    std::uint64_t moved = 0;
    while (moved != buffer.size()) {
        OVERLAPPED position {};
        std::uint64_t const at = offset + moved;
        position.Offset = static_cast<DWORD>(at & 0xFFFFFFFFull);
        position.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD arrived = 0;
        DWORD const asking = static_cast<DWORD>(buffer.size() - moved);
        if (!ReadFile(file, buffer.data() + moved, asking, &arrived, &position)) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return status_t {status_code_t::input_output_k, "A positioned read failed"};
        }
        if (arrived == 0) break;
        moved += arrived;
    }
    return moved;
}

/** Writes at most @p buffer bytes at @p offset, answering how many landed. */
inline expected<std::uint64_t> write_native_file(native_file_t file, std::uint64_t offset,
                                                 std::span<byte_t const> buffer) noexcept {
    std::uint64_t moved = 0;
    while (moved != buffer.size()) {
        OVERLAPPED position {};
        std::uint64_t const at = offset + moved;
        position.Offset = static_cast<DWORD>(at & 0xFFFFFFFFull);
        position.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD landed = 0;
        DWORD const asking = static_cast<DWORD>(buffer.size() - moved);
        if (!WriteFile(file, buffer.data() + moved, asking, &landed, &position))
            return status_t {status_code_t::input_output_k, "A positioned write failed"};
        if (landed == 0) break;
        moved += landed;
    }
    return moved;
}

/** Makes every landed write durable. */
inline status_t flush_native_file(native_file_t file) noexcept {
    if (!FlushFileBuffers(file)) return {status_code_t::input_output_k, "The file would not flush"};
    return {};
}

/** The bytes the file currently holds. */
[[nodiscard]] inline std::uint64_t native_file_capacity(native_file_t file) noexcept {
    LARGE_INTEGER size {};
    return GetFileSizeEx(file, &size) ? static_cast<std::uint64_t>(size.QuadPart) : 0;
}

/** Resizes the file to @p bytes, which a mount does once to preallocate a volume. */
inline status_t truncate_native_file(native_file_t file, std::uint64_t bytes) noexcept {
    LARGE_INTEGER position {};
    position.QuadPart = static_cast<LONGLONG>(bytes);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN) || !SetEndOfFile(file))
        return {status_code_t::input_output_k, "The file would not resize"};
    return {};
}

#else

/** The registered medium one open file is addressed by. */
using native_file_t = int;

/** The registered medium value that marks a file that never opened. */
inline native_file_t const invalid_native_file_k = -1;

/** Translates the platform's failure into the status the ring reports. */
inline status_t native_file_failure(int reason, char const* message) noexcept {
    switch (reason) {
    case ENOSPC:
    case EDQUOT: return {status_code_t::capacity_exhausted_k, message};
    case ENOENT: return {status_code_t::not_found_k, message};
    case EEXIST: return {status_code_t::already_exists_k, message};
    case EINVAL:
    case EBADF: return {status_code_t::invalid_argument_k, message};
    case ENOMEM: return {status_code_t::out_of_memory_k, message};
    case EIO: return {status_code_t::input_output_k, message};
    default: return {status_code_t::device_k, message};
    }
}

#if defined(__linux__)

/** The open flag that takes the page cache out of the path. */
[[nodiscard]] inline int unbuffered_open_flag() noexcept { return O_DIRECT; }

/** The bytes a regular file holds, or the whole medium behind a block device, which @c fstat sizes at zero. */
[[nodiscard]] inline std::uint64_t native_file_capacity(native_file_t file) noexcept {
    struct stat description {};
    if (::fstat(file, &description) != 0) return 0;
    if (!S_ISBLK(description.st_mode)) return static_cast<std::uint64_t>(description.st_size);
    std::uint64_t bytes = 0;
    return ::ioctl(file, BLKGETSIZE64, &bytes) == 0 ? bytes : 0;
}

#else

/** The open flag that takes the page cache out of the path, which this platform sets afterwards. */
[[nodiscard]] inline int unbuffered_open_flag() noexcept { return 0; }

/** The bytes a regular file holds, which is all this platform's @c fstat answers. */
[[nodiscard]] inline std::uint64_t native_file_capacity(native_file_t file) noexcept {
    struct stat description {};
    return ::fstat(file, &description) == 0 ? static_cast<std::uint64_t>(description.st_size) : 0;
}

#endif

#if defined(__APPLE__)

/** Asks for unbuffered access on a platform whose open flag cannot express it. */
inline void request_unbuffered_access(native_file_t file) noexcept { fcntl(file, F_NOCACHE, 1); }

/** Makes every landed write durable, past the drive's own cache. */
inline status_t flush_native_file(native_file_t file) noexcept {
    if (fcntl(file, F_FULLFSYNC) != 0) return native_file_failure(errno, "The file would not flush");
    return {};
}

#else

/** Asks for unbuffered access, which this platform already granted at open. */
inline void request_unbuffered_access(native_file_t) noexcept {}

/** Makes every landed write durable, without waiting on the metadata that does not matter. */
inline status_t flush_native_file(native_file_t file) noexcept {
    if (fdatasync(file) != 0) return native_file_failure(errno, "The file would not flush");
    return {};
}

#endif

/** Opens @p path, or reports why the platform refused. */
inline expected<native_file_t> open_native_file(char const* path, medium_access_t access,
                                                file_buffering_t buffering) noexcept {
    int flags = access == medium_access_t::read_only_k ? O_RDONLY : O_RDWR;
    if (access == medium_access_t::create_read_write_k) flags |= O_CREAT;
    if (buffering == file_buffering_t::unbuffered_k) flags |= unbuffered_open_flag();
    native_file_t const file = ::open(path, flags, 0644);
    if (file == invalid_native_file_k) return native_file_failure(errno, "The file would not open");
    if (buffering == file_buffering_t::unbuffered_k) request_unbuffered_access(file);
    return file;
}

/** Closes @p file, which must have opened. */
inline void close_native_file(native_file_t file) noexcept { ::close(file); }

/** Reads at most @p buffer bytes from @p offset, answering how many arrived. */
inline expected<std::uint64_t> read_native_file(native_file_t file, std::uint64_t offset,
                                                std::span<byte_t> buffer) noexcept {
    std::uint64_t moved = 0;
    while (moved != buffer.size()) {
        ssize_t const arrived = ::pread(file, buffer.data() + moved, buffer.size() - moved,
                                        static_cast<off_t>(offset + moved));
        if (arrived < 0) {
            if (errno == EINTR) continue;
            return native_file_failure(errno, "A positioned read failed");
        }
        if (arrived == 0) break;
        moved += static_cast<std::uint64_t>(arrived);
    }
    return moved;
}

/** Writes at most @p buffer bytes at @p offset, answering how many landed. */
inline expected<std::uint64_t> write_native_file(native_file_t file, std::uint64_t offset,
                                                 std::span<byte_t const> buffer) noexcept {
    std::uint64_t moved = 0;
    while (moved != buffer.size()) {
        ssize_t const landed = ::pwrite(file, buffer.data() + moved, buffer.size() - moved,
                                        static_cast<off_t>(offset + moved));
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

/** Resizes a regular file to @p bytes, which a mount does once to preallocate a volume; a block device is refused. */
inline status_t truncate_native_file(native_file_t file, std::uint64_t bytes) noexcept {
    struct stat description {};
    if (::fstat(file, &description) == 0 && S_ISBLK(description.st_mode))
        return {status_code_t::invalid_argument_k, "A block device keeps the capacity of its medium"};
    if (::ftruncate(file, static_cast<off_t>(bytes)) != 0)
        return native_file_failure(errno, "The file would not resize");
    return {};
}

#endif

#pragma endregion Platform

#pragma region Device

/**
 *  @brief One open file, read and written at absolute offsets by any number of threads at once.
 *
 *  Neither @c pread nor an @c OVERLAPPED @c ReadFile moves a shared cursor, so the device needs no
 *  lock of its own and a queue pair per thread is the only concurrency control. A short result is not an
 *  error here: a read that meets the end of the file, and a write that meets a full volume, answer
 *  the prefix they moved and let the ring report it as a partial transfer.
 */
class blocking_files_device_t {
  public:

    blocking_files_device_t() noexcept = default;
    blocking_files_device_t(blocking_files_device_t const&) = delete;
    blocking_files_device_t& operator=(blocking_files_device_t const&) = delete;

    blocking_files_device_t(blocking_files_device_t&& other) noexcept
        : files_(std::move(other.files_)), buffering_(other.buffering_) {}

    blocking_files_device_t& operator=(blocking_files_device_t&& other) noexcept {
        if (this == &other) return *this;
        close_all_();
        files_ = std::move(other.files_);
        buffering_ = other.buffering_;
        return *this;
    }

    ~blocking_files_device_t() noexcept { close_all_(); }

    /**
     *  @brief Makes a device that opens its files with @p buffering.
     *  @param[in] buffering Whether the page cache stands in the path, which fixes the alignments.
     *
     *  @note No file is opened here; a mount registers each one as a medium, so a directory of many
     *      sorted runs is one device rather than one device per run.
     */
    static expected<blocking_files_device_t> make(file_buffering_t buffering) noexcept {
        blocking_files_device_t device;
        device.buffering_ = buffering;
        return device;
    }

    /**
     *  @brief Opens the file @p request names, creating and sizing it when it asks for that.
     *  @param[in] request The path, the bytes to create it with, and what the caller may do to it.
     *  @return The slot the file took, which every request against it names.
     */
    expected<std::uint16_t> open_medium(medium_options_t const& request) noexcept {
        if (!request.name) return status_t {status_code_t::invalid_argument_k, "A file medium needs a path"};
        auto [file, open_native_file_status] = open_native_file(request.name, request.access, buffering_);
        if (open_native_file_status.failed()) return open_native_file_status;
        if (request.access == medium_access_t::create_read_write_k && request.length != 0)
            if (status_t const sized = truncate_native_file(file, request.length); sized.failed()) {
                close_native_file(file);
                return sized;
            }

        native_file_t opened = file;
        for (std::size_t index = 0; index != files_.size(); ++index)
            if (files_[index] == invalid_native_file_k) {
                files_[index] = opened;
                return static_cast<std::uint16_t>(index);
            }
        if (files_.push_back(std::move(opened)) != st::status_t::success_k) {
            close_native_file(file);
            return status_t {status_code_t::out_of_memory_k, "A device could not track another medium"};
        }
        return static_cast<std::uint16_t>(files_.size() - 1);
    }

    /** Closes the file in @p index, leaving its slot free for the next medium. */
    status_t close_medium(std::uint16_t index) noexcept {
        if (index >= files_.size() || files_[index] == invalid_native_file_k)
            return {status_code_t::invalid_argument_k, "The medium is not this device's"};
        close_native_file(files_[index]);
        files_[index] = invalid_native_file_k;
        return {};
    }

    /** Reads @p extent of @p index into @p destination, answering the prefix that arrived. */
    expected<std::uint64_t> read_extent(std::uint16_t index, extent_t extent, std::span<byte_t> destination) noexcept {
        if (index >= files_.size() || files_[index] == invalid_native_file_k)
            return status_t {status_code_t::invalid_argument_k, "The medium is not this device's"};
        return read_native_file(files_[index], extent.offset,
                                destination.first(static_cast<std::size_t>(extent.length)));
    }

    /** Writes @p source at @p extent of @p index, answering the prefix that landed. */
    expected<std::uint64_t> write_extent(std::uint16_t index, extent_t extent,
                                         std::span<byte_t const> source) noexcept {
        if (index >= files_.size() || files_[index] == invalid_native_file_k)
            return status_t {status_code_t::invalid_argument_k, "The medium is not this device's"};
        return write_native_file(files_[index], extent.offset, source.first(static_cast<std::size_t>(extent.length)));
    }

    /** Makes every write @p index has already answered durable. */
    status_t flush(std::uint16_t index) noexcept {
        if (index >= files_.size() || files_[index] == invalid_native_file_k)
            return {status_code_t::invalid_argument_k, "The medium is not this device's"};
        return flush_native_file(files_[index]);
    }

    /** The bytes @p index currently holds: a regular file grows past it, a block device falls short. */
    [[nodiscard]] std::uint64_t capacity(std::uint16_t index) const noexcept {
        if (index >= files_.size() || files_[index] == invalid_native_file_k) return 0;
        return native_file_capacity(files_[index]);
    }

    /** Preallocates the regular file in @p index to @p bytes, which a mount does once; a block device is refused. */
    status_t resize(std::uint16_t index, std::uint64_t bytes) noexcept {
        if (index >= files_.size() || files_[index] == invalid_native_file_k)
            return {status_code_t::invalid_argument_k, "The medium is not this device's"};
        return truncate_native_file(files_[index], bytes);
    }

    /** What this transport accepts, which unbuffered access tightens to the portable alignment. */
    [[nodiscard]] ring_capabilities_t capabilities() const noexcept {
        ring_capabilities_t reported;
        std::uint64_t const alignment = buffering_ == file_buffering_t::unbuffered_k ? portable_alignment_k : 1;
        reported.memory_alignment = alignment;
        reported.offset_alignment = alignment;
        reported.length_granularity = alignment;
        reported.max_transfer_bytes = max_transfer_bytes_k;
        reported.reaches_gpu_memory = false;
        return reported;
    }

  private:

    /** The largest extent one positioned call carries, kept well inside every platform's own limit. */
    static constexpr std::uint64_t max_transfer_bytes_k = 1024ull * 1024ull * 1024ull;

    /** Closes every open file, which both the destructor and a move assignment need. */
    void close_all_() noexcept {
        for (std::size_t index = 0; index != files_.size(); ++index)
            if (files_[index] != invalid_native_file_k) close_native_file(files_[index]);
        files_.clear();
    }

    /** One entry per registered medium, @c invalid_native_file_k where a slot is free. */
    st::basic_vector<native_file_t> files_;

    /** Whether the page cache stands in the path, which fixes the reported alignments. */
    file_buffering_t buffering_ = file_buffering_t::buffered_k;
};

/** The ring every platform has: positioned reads and writes, performed on the thread that polls. */
using blocking_files_disk_t = basic_disk<blocking_files_device_t>;

#pragma endregion Device

} // namespace unum::ustore::nvme
