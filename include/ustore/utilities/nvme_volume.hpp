/**
 *  @file include/ustore/utilities/nvme_volume.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Volumes: where the runs of one database are placed across its data directories, and how
 *      the manifest naming them is committed.
 *
 *  A volume spans every data directory of a database and places each run on the medium with the
 *  most free bytes. Neither volume keeps a container file:
 *  - @c basic_directory_volume keeps one @c <database>-<run id>.ustore file per run, its own
 *    registered medium, and commits @c <database>.ustore.json by flushing a sibling copy and
 *    renaming it over the manifest, then syncs the directory and unlinks consumed files.
 *  - @c basic_slotted_volume cuts raw block devices and SPDK namespaces into slots of
 *    @c slot_bytes_k. The slot at data directory 0's base holds the manifest in one of two JSON
 *    regions, and the commit rewrites that slot's owner block to point at the region just written.
 *
 *  A run written into a volume is durable only once a manifest naming it is published: the volume's
 *  @c publish_manifest flushes every medium written since the last one before it commits, which is
 *  two flushes per publication however many runs it names. Neither volume is on the read path: a
 *  run is placed once, and every block read afterwards is an extent plus a submit.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint16_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <algorithm>   // `std::binary_search`
#include <charconv>    // `std::from_chars`, `std::to_chars`
#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::conditional_t`, `std::invoke_result_t`
#include <utility>     // `std::move`, `std::exchange`
#include <variant>     // `std::variant`, `std::monostate`, `std::holds_alternative`, `std::get_if`

#include <smashtable/basic_growing_pool.hpp> // `st::basic_growing_pool`
#include <smashtable/basic_fixed_pool.hpp>   // `st::basic_fixed_pool`
#include <smashtable/basic_vector.hpp>       // `st::vector`

#include "ustore/utilities/algorithm.hpp"      // `radix_sort`
#include "ustore/utilities/blocking_files.hpp" // `replace_file_durably`, `read_file`, `list_directory`, `temporary_path_of`
#include "ustore/utilities/nvme_channels.hpp"  // `io_channel_t`, `basic_read_pipeline`, `basic_write_pipeline`
#include "ustore/utilities/nvme_manifest.hpp"  // `manifest_t`, `manifest_file_t`
#include "ustore/utilities/nvme_ring.hpp"      // `extent_t`, `ring_concept`, `registered_medium_t`
#include "ustore/utilities/nvme_slots.hpp"     // `slot_t`, `slot_owner_block_t`, `slot_bytes_k`
#include "ustore/utilities/types.hpp"          // `status_t`, `status_code_t`, `expected`, `uuid128_t`

namespace unum::ustore::utilities {

#pragma region Placement

/** Where one run lies: the medium requests address, the bytes inside it, and the volume's
 *  record of it. */
struct medium_extent_t {

    /** The medium every read and write of the run names. */
    medium_reference_t medium;

    /** The run's bytes, absolute inside that medium. */
    extent_t extent;

    /** The data directory holding the run, which a manifest prints as @c medium. */
    std::uint16_t data_directory = 0;

    /** The volume's record of the placement: a slot on raw media, a run file on a filesystem. */
    placement_handle_t placement;

    /** The part of the run @p offset bytes in, as the extent a request carries. */
    [[nodiscard]] constexpr extent_t slice(std::uint64_t offset, std::uint64_t bytes) const noexcept {
        extent_t const inside = extent_t {offset, bytes}.clipped_to(extent.length);
        return extent_t {extent.offset + inside.offset, inside.length};
    }
};

/** What a data directory address names. */
enum class data_address_kind_t : std::uint8_t {

    /** A directory of a filesystem, which a directory volume keeps run files in. */
    directory_k,

    /** A block device or a file standing in for one, from a base address on, which a slotted volume
     *  cuts into slots. */
    raw_k,

    /** A namespace of an SPDK controller, which a slotted volume cuts into slots. */
    spdk_k,
};

/**
 *  @brief One data directory as its address names it, in any of these forms:
 *
 *  @verbatim
 *  /mnt/nvme0/molecules            a directory
 *  /dev/nvme0n1@64GiB              a device node from byte 64 GiB
 *  /dev/nvme0n1@64GiB+16GiB        the 16 GiB of it from byte 64 GiB on
 *  spdk://0000:5e:00.0/2@0GiB      namespace 2 of a local PCIe controller
 *  @endverbatim
 *
 *  A base and a length are whole numbers of B, KiB, MiB, GiB or TiB, all binary, bytes when bare.
 */
struct data_address_t {

    /** The directory, the device node or file, or the PCIe address of an SPDK controller. */
    std::string_view location;

    /** The namespace of an SPDK controller, one by default. */
    std::uint32_t namespace_id = 0;

    /** Where the database's first slot begins on a raw medium. */
    std::uint64_t byte_base = 0;

    /** Bytes past the base the database may take, zero meaning the whole medium. */
    std::uint64_t byte_limit = 0;

    /** What the address names. */
    data_address_kind_t kind = data_address_kind_t::directory_k;
};

/** Reads @p text, a whole decimal number and an optional binary unit, into @p bytes. */
[[nodiscard]] inline bool parse_byte_count(std::string_view text, std::uint64_t& bytes) noexcept {
    std::uint64_t count = 0;
    std::from_chars_result const result = std::from_chars(text.data(), text.data() + text.size(), count);
    if (result.ec != std::errc {}) return false;
    std::string_view const suffix = text.substr(static_cast<std::size_t>(result.ptr - text.data()));
    std::string_view const unit = suffix.empty() ? "B" : suffix;
    constexpr std::string_view units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    unsigned shift = 0;
    for (std::string_view const known : units) {
        if (unit == known) {
            if (count > ~std::uint64_t {0} >> shift) return false;
            bytes = count << shift;
            return true;
        }
        shift += 10;
    }
    return false;
}

/** The suffix of every manifest on a filesystem, which the database's name precedes. */
inline constexpr std::string_view manifest_suffix_k = ".ustore.json";

/** Splits @p address into a @c data_address_t, refusing a base, a length or a namespace that
 *  does not read. */
inline expected<data_address_t> parse_data_address(std::string_view address) noexcept {
    data_address_t parsed;
    std::string_view location = address;
    std::size_t const at = location.rfind('@');
    if (at != std::string_view::npos && at + 1 < location.size() && location[at + 1] >= '0' &&
        location[at + 1] <= '9') {
        std::string_view const extent = location.substr(at + 1);
        std::size_t const plus = extent.find('+');
        if ((plus != std::string_view::npos && !parse_byte_count(extent.substr(plus + 1), parsed.byte_limit)) ||
            !parse_byte_count(extent.substr(0, plus), parsed.byte_base))
            return status_t {status_code_t::invalid_argument_k, "A data directory's base or length does not read"};
        location = location.substr(0, at);
        parsed.kind = data_address_kind_t::raw_k;
    }
    std::string_view const scheme = "spdk://";
    if (location.starts_with(scheme)) {
        location.remove_prefix(scheme.size());
        std::size_t const slash = location.find('/');
        parsed.namespace_id = 1;
        if (slash != std::string_view::npos) {
            std::string_view const number = location.substr(slash + 1);
            std::from_chars_result const result = std::from_chars(number.data(), number.data() + number.size(),
                                                                  parsed.namespace_id);
            if (result.ec != std::errc {} || result.ptr != number.data() + number.size())
                return status_t {status_code_t::invalid_argument_k, "An SPDK namespace does not read"};
            location = location.substr(0, slash);
        }
        parsed.kind = data_address_kind_t::spdk_k;
    }
    parsed.location = location;
    if (location.empty()) return status_t {status_code_t::invalid_argument_k, "A data directory names no location"};
    return parsed;
}

/** What a volume is mounted over. */
struct volume_config_t {

    /** The database's name, which prefixes every run file and names the manifest of
     *  a directory volume. */
    string_view_t database;

    /** Every data directory, the first holding the manifest; all of one kind. */
    std::span<data_address_t const> data_directories;

    /** Run files a directory volume keeps registered at once, which bounds the runs
     *  of the database. */
    std::size_t runs_capacity = 0;
};

#pragma endregion Placement

#pragma region Extent Bytes

/** Stages @p bytes at the start of @p extent, zero-padded to a page, one channel block at a
 *  time, and drains the writes without a flush, which the publication of the manifest naming the
 *  extent pays for. */
template <typename disk_type_, typename waiter_type_>
status_t write_extent(disk_type_& disk, io_channel_t& channel, medium_extent_t const& extent,
                      std::span<byte_t const> bytes) noexcept {
    auto [writer_channel, writer_channel_status] = channel.acquire();
    if (writer_channel_status.failed()) return writer_channel_status;
    basic_write_pipeline<disk_type_, waiter_type_> writer {disk, std::move(writer_channel), extent.medium};
    std::uint64_t const total = round_up_to_multiple<std::uint64_t>(bytes.size(), portable_alignment_k);
    if (total > extent.extent.length) return {status_code_t::invalid_argument_k, "The bytes outgrow their extent"};
    for (std::uint64_t written = 0; written != total;) {
        auto [block, acquire_status] = writer.acquire_buffer_or_poll();
        if (acquire_status.failed()) return acquire_status;
        std::uint64_t const room = round_down_to_multiple<std::uint64_t>(block.size(), portable_alignment_k);
        if (room == 0) return {status_code_t::capacity_exhausted_k, "A channel's block holds no page"};
        std::uint64_t const chunk = total - written < room ? total - written : room;
        std::uint64_t const left = bytes.size() - written;
        std::uint64_t const carried = left < chunk ? left : chunk;
        std::span<byte_t> const target = block.bytes();
        std::memcpy(target.data(), bytes.data() + written, static_cast<std::size_t>(carried));
        std::memset(target.data() + carried, 0, static_cast<std::size_t>(chunk - carried));
        if (status_t const queued = writer.enqueue_write(std::move(block), extent.slice(written, chunk));
            queued.failed())
            return queued;
        if (status_t const submitted = writer.submit(); submitted.failed()) return submitted;
        written += chunk;
    }
    return writer.poll_all_completions();
}

/** Reads the first @p into.size() bytes of @p extent into @p into, a whole number of pages
 *  at a time. */
template <typename disk_type_>
status_t read_extent(disk_type_& disk, io_channel_t& channel, medium_extent_t const& extent,
                     std::span<byte_t> into) noexcept {
    auto [reader_channel, reader_channel_status] = channel.acquire();
    if (reader_channel_status.failed()) return reader_channel_status;
    basic_read_pipeline<disk_type_> reader {disk, std::move(reader_channel), extent.medium};
    std::uint64_t const block_bytes = round_down_to_multiple<std::uint64_t>(channel.requests().buffer_bytes(),
                                                                            portable_alignment_k);
    if (block_bytes == 0) return {status_code_t::capacity_exhausted_k, "A channel's block holds no page"};
    status_t carried;
    auto sink = [&](completed_buffer_t const& completed) noexcept {
        if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
            carried = whole;
            return;
        }
        std::uint64_t const begin = completed.tag * block_bytes;
        std::uint64_t const length = into.size() - begin < completed.bytes.size() ? into.size() - begin
                                                                                  : completed.bytes.size();
        std::memcpy(into.data() + begin, completed.bytes.data(), static_cast<std::size_t>(length));
    };
    std::uint64_t const total = round_up_to_multiple<std::uint64_t>(into.size(), portable_alignment_k);
    request_tag_t tag = 0;
    for (std::uint64_t offered = 0; offered < total; offered += block_bytes, ++tag) {
        std::uint64_t const chunk = total - offered < block_bytes ? total - offered : block_bytes;
        while (!reader.can_enqueue())
            if (status_t const polled = reader.poll_one_completion(sink); polled.failed()) return polled;
        if (status_t const queued = reader.enqueue_read(extent.slice(offered, chunk), tag); queued.failed())
            return queued;
        if (status_t const submitted = reader.submit(); submitted.failed()) return submitted;
    }
    if (status_t const polled = reader.poll_all_completions(sink); polled.failed()) return polled;
    return carried;
}

#pragma endregion Extent Bytes

#pragma region Slot Allocator

/** What became of a slot since the last publication, which tells whether its owner block is
 *  written or cleared. */
enum class slot_change_t : bool { vacated_k, taken_k };

/**
 *  @brief The slots of one raw medium, taken and given back through a SmashTable slot pool.
 *
 *  Runs under @c packed_run_bytes_max_k are appended into the open packed slot, which is vacated
 *  once nothing in it is live; longer runs take a whole slot. Every slot taken or vacated is
 *  marked, so the volume writes or zeroes its owner block at the next publication.
 */
template <typename allocator_type_>
class basic_slot_allocator {
  public:

    using slot_pool_t = st::basic_fixed_pool<slot_t, std::uint32_t, allocator_type_>;
    static constexpr std::uint32_t absent_k = slot_pool_t::absent_k;

    basic_slot_allocator() noexcept = default;
    basic_slot_allocator(basic_slot_allocator&&) noexcept = default;
    basic_slot_allocator& operator=(basic_slot_allocator&&) noexcept = default;

    /** A pool of @p count slots of data directory @p medium, every one taken until a mount releases
     *  the free ones. */
    static expected<basic_slot_allocator> make(std::uint16_t medium, std::size_t count,
                                               allocator_type_ allocator) noexcept {
        basic_slot_allocator made;
        auto [pool, make_status] = slot_pool_t::make(count, std::move(allocator));
        if (make_status != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room for the slots of a medium"};
        made.slots_ = std::move(pool);
        for (std::size_t index = 0; index != count; ++index) [[maybe_unused]]
            std::uint32_t const taken = made.slots_.take();
        if (made.touched_.resize((count + 63) / 64, 0) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room for the slots of a medium"};
        made.medium_ = medium;
        return made;
    }

    /** Places a run of @p bytes, with its offset counted from the medium's base. */
    expected<medium_extent_t> allocate(std::uint64_t bytes) noexcept {
        std::uint64_t const rounded = round_up_to_multiple(bytes, portable_alignment_k);
        if (rounded == 0 || rounded > sorted_run_bytes_max_k)
            return status_t {status_code_t::invalid_argument_k, "A run fits one slot"};
        bool const packed = rounded < packed_run_bytes_max_k;
        std::uint32_t slot = packed ? open_ : absent_k;
        if (slot == absent_k || slots_[slot].tail_bytes + rounded > slot_bytes_k) {
            std::uint32_t const taken = slots_.take();
            if (taken == absent_k) return status_t {status_code_t::capacity_exhausted_k, "No free slot"};
            slots_[taken].tail_bytes = static_cast<std::uint32_t>(slot_payload_offset_k);
            touch_(taken);
            if (packed) {
                if (open_ != absent_k && slots_[open_].live_bytes == 0) vacate_(open_);
                open_ = taken;
            }
            slot = taken;
        }
        slot_t& held = slots_[slot];
        medium_extent_t placed;
        placed.extent = extent_t {std::uint64_t {slot} * slot_bytes_k + held.tail_bytes, rounded};
        placed.data_directory = medium_;
        placed.placement = handle_of(slot);
        held.tail_bytes += static_cast<std::uint32_t>(rounded);
        held.live_bytes += static_cast<std::uint32_t>(rounded);
        return placed;
    }

    /** Gives @p extent back once the manifest that stopped naming it is durable. */
    void deallocate(medium_extent_t const& extent) noexcept {
        slot_t* const slot = slots_.at(
            typename slot_pool_t::handle_t {extent.placement.index, extent.placement.generation});
        if (!slot) return;
        slot->live_bytes -= static_cast<std::uint32_t>(extent.extent.length);
        if (slot->live_bytes == 0 && extent.placement.index != open_) vacate_(extent.placement.index);
    }

    /** Marks @p length bytes from @p offset inside slot @p index live, as a mount does for every
     *  run the manifest names. */
    void credit(std::uint32_t index, std::uint64_t offset, std::uint64_t length) noexcept {
        slot_t& slot = slots_[index];
        slot.live_bytes += static_cast<std::uint32_t>(length);
        // A run of a slot of its own leaves no room a packed run may take.
        std::uint64_t const tail = length < packed_run_bytes_max_k ? offset + length : slot_bytes_k;
        if (tail > slot.tail_bytes) slot.tail_bytes = static_cast<std::uint32_t>(tail);
    }

    /** Reopens the held packed slot with the most room left, so a mount keeps packing where the
     *  last one stopped. */
    void reopen_packed() noexcept {
        std::uint32_t chosen = absent_k;
        for (std::uint32_t index = 0; index != slots_.capacity(); ++index) {
            slot_t const& slot = slots_[index];
            if (slot.live_bytes == 0 || slot.tail_bytes >= slot_bytes_k) continue;
            if (chosen == absent_k || slot.tail_bytes < slots_[chosen].tail_bytes) chosen = index;
        }
        open_ = chosen;
    }

    /** Frees slot @p index, which a mount does for a zero owner block and a crash leftover,
     *  in descending order. */
    void release(std::uint32_t index) noexcept {
        slots_[index].live_bytes = 0;
        slots_[index].tail_bytes = 0;
        slots_.vacate(index);
    }

    /** Whether the slot @p placement names is still held under the generation it was placed at. */
    [[nodiscard]] bool holds(placement_handle_t placement) const noexcept {
        return slots_.at(typename slot_pool_t::handle_t {placement.index, placement.generation}) != nullptr;
    }

    /** The handle of slot @p index as it stands. */
    [[nodiscard]] placement_handle_t handle_of(std::uint32_t index) const noexcept {
        return placement_handle_t {index, slots_[index].generation};
    }

    /** The record of slot @p index. */
    [[nodiscard]] slot_t const& slot(std::uint32_t index) const noexcept { return slots_[index]; }

    /** Slots of the medium, taken or free. */
    [[nodiscard]] std::size_t size() const noexcept { return slots_.capacity(); }

    /** Bytes still placeable: whole free slots and the open packed slot's room. */
    [[nodiscard]] std::uint64_t free_bytes() const noexcept {
        std::uint64_t free = (slots_.capacity() - slots_.size()) * sorted_run_bytes_max_k;
        if (open_ != absent_k) free += slot_bytes_k - slots_[open_].tail_bytes;
        return free;
    }

    /** Calls @p visitor with every slot taken or vacated since the last call and what became of it,
     *  and forgets them. */
    template <typename visitor_type_>
    status_t drain_touched(visitor_type_&& visitor) noexcept {
        for (std::size_t word = 0; word != touched_.size(); ++word)
            for (std::uint64_t bits = std::exchange(touched_[word], 0); bits != 0; bits &= bits - 1) {
                std::uint32_t const index = static_cast<std::uint32_t>(word * 64 + std::countr_zero(bits));
                slot_change_t const change = slots_[index].tail_bytes != 0 ? slot_change_t::taken_k
                                                                           : slot_change_t::vacated_k;
                if (status_t const visited = visitor(index, change); visited.failed()) return visited;
            }
        return {};
    }

  private:

    void touch_(std::uint32_t index) noexcept { touched_[index / 64] |= std::uint64_t {1} << (index % 64); }

    void vacate_(std::uint32_t index) noexcept {
        if (index == open_) open_ = absent_k;
        release(index);
        touch_(index);
    }

    slot_pool_t slots_;

    /** One bit per slot whose owner block awaits a write or a zeroing. */
    st::vector<std::uint64_t> touched_;

    /** The packed slot small runs are appended to, or @c absent_k. */
    std::uint32_t open_ = absent_k;

    /** The data directory these slots belong to. */
    std::uint16_t medium_ = 0;
};

#pragma endregion Slot Allocator

#pragma region Directory Volume

/**
 *  @brief Runs as @c <database>-<run id>.ustore files across directories of filesystems, named by a
 *      @c <database>.ustore.json manifest in the first directory.
 *  @tparam allocator_type_ The byte allocator the run-file pool and the manifest are charged to.
 */
template <typename disk_type_, typename waiter_type_, typename allocator_type_>
    requires ring_concept<disk_type_>
class basic_directory_volume {
  public:

    using disk_t = disk_type_;
    using allocator_t = allocator_type_;

    basic_directory_volume() noexcept = default;
    basic_directory_volume(basic_directory_volume const&) = delete;
    basic_directory_volume& operator=(basic_directory_volume const&) = delete;

    basic_directory_volume(basic_directory_volume&& other) noexcept
        : disk_(std::exchange(other.disk_, nullptr)), allocator_(std::move(other.allocator_)),
          directories_(std::move(other.directories_)), run_files_(std::move(other.run_files_)),
          unflushed_(std::move(other.unflushed_)), unpublished_(std::move(other.unpublished_)),
          manifest_(std::move(other.manifest_)), lock_(std::move(other.lock_)) {}

    basic_directory_volume& operator=(basic_directory_volume&& other) noexcept {
        if (this == &other) return *this;
        [[maybe_unused]] status_t const released = unmount();
        disk_ = std::exchange(other.disk_, nullptr);
        allocator_ = std::move(other.allocator_);
        directories_ = std::move(other.directories_);
        run_files_ = std::move(other.run_files_);
        unflushed_ = std::move(other.unflushed_);
        unpublished_ = std::move(other.unpublished_);
        manifest_ = std::move(other.manifest_);
        lock_ = std::move(other.lock_);
        return *this;
    }

    ~basic_directory_volume() noexcept { [[maybe_unused]] status_t const released = unmount(); }

    /**
     *  @brief Opens every directory of @p config, reads the manifest when there is one, and unlinks
     *      every run file of this database the manifest does not name, which a crash left behind.
     *  @return The volume, @c invalid_argument_k for an address that names no directory,
     *      @c contended_k while another mount holds the manifest's directory, or @c malformed_k for
     *      a manifest of another database or one naming a directory this mount did not list.
     */
    static expected<basic_directory_volume> mount(disk_type_& disk, [[maybe_unused]] io_channel_t& channel,
                                                  volume_config_t const& config, allocator_type_ allocator) noexcept {
        if (config.data_directories.empty() || config.data_directories.size() > limit_of_<std::uint16_t>())
            return status_t {status_code_t::invalid_argument_k, "A volume spans one to 65,535 data directories"};
        if (config.database.empty()) return status_t {status_code_t::invalid_argument_k, "A database needs a name"};
        basic_directory_volume volume {disk, allocator};
        if (volume.directories_.reserve(config.data_directories.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room for the data directories"};
        for (data_address_t const& address : config.data_directories) {
            if (address.kind != data_address_kind_t::directory_k)
                return status_t {status_code_t::invalid_argument_k, "A directory volume spans directories alone"};
            directory_t directory;
            directory.quota = address.byte_limit;
            if (sz::failed(directory.path.assign(string_view_t {address.location})))
                return status_t {status_code_t::out_of_memory_k, "No room for a data directory's path"};
            if (status_t const made = create_directories(directory.path.c_str()); made.failed()) return made;
            [[maybe_unused]] st::status_t const placed = volume.directories_.push_back(st::assume_reserved,
                                                                                       std::move(directory));
        }
        // A second mount would sweep the first one's unpublished runs as leftovers.
        auto [locked, lock_status] = file_lock_t::acquire(volume.directories_[0].path.c_str());
        if (lock_status.failed()) return lock_status;
        volume.lock_ = std::move(locked);
        // The records grow in chunks that never move, so a small database pays for few of them.
        auto [files, make_status] = run_files_t::make(1, config.runs_capacity, allocator);
        if (make_status != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room for the run files"};
        volume.run_files_ = std::move(files);

        if (sz::failed(volume.manifest_.database.assign(config.database)))
            return status_t {status_code_t::out_of_memory_k, "No room for the database's name"};
        char path[path_max_bytes_k];
        if (status_t const named = volume.manifest_path_(path); named.failed()) return named;
        st::vector<char> text;
        status_t const read = read_file(path, text);
        if (read == status_code_t::not_found_k) volume.manifest_.id = uuid_generator();
        else if (read.failed()) return read;
        else if (status_t const parsed = volume.manifest_.parse(text, allocator); parsed.failed()) return parsed;
        if (volume.manifest_.database != config.database || volume.manifest_.placement != manifest_placement_t::paths_k)
            return status_t {status_code_t::malformed_k, "The manifest names another database or no paths"};
        for (manifest_file_t const& file : volume.manifest_.files)
            if (file.medium >= volume.directories_.size())
                return status_t {status_code_t::malformed_k, "The manifest names a data directory this mount lacks"};
        if (status_t const swept = volume.sweep_leftovers_(); swept.failed()) return swept;
        return volume;
    }

    /** The manifest the mount read, or an empty one naming the database's fresh ID. */
    [[nodiscard]] manifest_t const& manifest() const noexcept { return manifest_; }

    /** How many manifests have been published, whose successor the next publication prints. */
    [[nodiscard]] std::uint64_t generation() const noexcept { return manifest_.generation; }

    /** How the manifests of this volume name their files. */
    [[nodiscard]] static constexpr manifest_placement_t placement() noexcept { return manifest_placement_t::paths_k; }

    /** Opens the run file @p file names, which the manifest the mount read lists. */
    expected<medium_extent_t> open(manifest_file_t const& file) noexcept {
        return register_run_(file.medium, file.run, 0, medium_access_t::read_write_k,
                             round_up_to_multiple(file.bytes, portable_alignment_k));
    }

    /** Creates @c <database>-<run>.ustore of @p bytes, rounded to a page, in the directory with the
     *  most free bytes; the file is durable only once a manifest naming it is published. */
    expected<medium_extent_t> allocate(uuid128_t const& run, std::uint64_t bytes) noexcept {
        std::uint64_t const rounded = round_up_to_multiple(bytes, portable_alignment_k);
        if (rounded == 0 || rounded > sorted_run_bytes_max_k)
            return status_t {status_code_t::invalid_argument_k, "A run fits one slot"};
        std::size_t chosen = directories_.size();
        std::uint64_t chosen_free = 0;
        for (std::size_t index = 0; index != directories_.size(); ++index) {
            std::uint64_t const free = free_bytes_(index);
            if (free < rounded || (chosen != directories_.size() && free <= chosen_free)) continue;
            chosen = index;
            chosen_free = free;
        }
        if (chosen == directories_.size())
            return status_t {status_code_t::capacity_exhausted_k, "No data directory has room for that run"};
        if (unflushed_.reserve(unflushed_.size() + 1) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room to track another run file"};
        auto [placed, register_status] = register_run_(static_cast<std::uint16_t>(chosen), run, rounded,
                                                       medium_access_t::create_read_write_k, rounded);
        if (register_status.failed()) return register_status;
        [[maybe_unused]] st::status_t const tracked = unflushed_.push_back(st::assume_reserved,
                                                                           placement_handle_t {placed.placement});
        return placed;
    }

    /** Closes and unlinks the file of @p extent, once the manifest that stopped naming
     *  it is durable. */
    void deallocate(medium_extent_t const& extent) noexcept {
        run_file_t* const file = run_files_.at(handle_of_(extent.placement));
        if (!file) return;
        [[maybe_unused]] status_t const closed = disk_->unregister_medium(file->medium);
        char path[path_max_bytes_k];
        if (run_path_(file->data_directory, file->run, path).succeeded()) [[maybe_unused]]
            status_t const removed = remove_file(path);
        directories_[file->data_directory].used -= file->bytes;
        run_files_.vacate(extent.placement.index);
    }

    /** Gives @p extent back after the next publication that succeeds, for a file the manifest on
     *  disk may still name, as one whose publication failed past its rename does. */
    void deallocate_after_publication(medium_extent_t const& extent) noexcept {
        // Without room the file stays until the next mount sweeps it, which only costs
        // its bytes meanwhile.
        [[maybe_unused]] st::status_t const remembered = unpublished_.push_back(medium_extent_t {extent});
    }

    /** Flushes every run file written since the last publication and every directory that gained
     *  one, then replaces the manifest with @p json, and gives back the files earlier
     *  publications stopped naming. */
    status_t publish_manifest(io_channel_t& channel, std::span<char const> json) noexcept {
        for (std::size_t index = 0; index != directories_.size(); ++index) {
            bool gained = false;
            for (placement_handle_t const& handle : unflushed_) {
                run_file_t* const file = run_files_.at(handle_of_(handle));
                if (!file || file->data_directory != index) continue;
                gained = true;
                auto [writer_channel, writer_channel_status] = channel.acquire();
                if (writer_channel_status.failed()) return writer_channel_status;
                basic_write_pipeline<disk_type_, waiter_type_> writer {*disk_, std::move(writer_channel),
                                                                       file->medium.reference()};
                if (status_t const durable = writer.make_durable(); durable.failed()) return durable;
            }
            if (gained)
                if (status_t const synced = sync_directory(directories_[index].path.c_str()); synced.failed())
                    return synced;
        }
        unflushed_.clear();
        char path[path_max_bytes_k];
        if (status_t const named = manifest_path_(path); named.failed()) return named;
        if (status_t const replaced = replace_file_durably(path, json); replaced.failed()) return replaced;
        ++manifest_.generation;
        for (medium_extent_t const& extent : unpublished_) deallocate(extent);
        unpublished_.clear();
        return {};
    }

    /** Writes the path of the file of @p run in data directory @p medium into @p into, which
     *  an archive links. */
    status_t file_path(std::uint16_t medium, uuid128_t const& run, std::span<char> into) const noexcept {
        if (medium >= directories_.size())
            return {status_code_t::invalid_argument_k, "The volume holds no data directory of that index"};
        return run_path_(medium, run, into);
    }

    /** Run files this volume keeps registered at most. */
    [[nodiscard]] std::size_t runs_capacity() const noexcept { return run_files_.capacity_limit(); }

    /** Bytes every data directory still offers. */
    [[nodiscard]] std::uint64_t free_bytes() const noexcept {
        std::uint64_t free = 0;
        for (std::size_t index = 0; index != directories_.size(); ++index) free += free_bytes_(index);
        return free;
    }

    /** The transport every read and write of a run goes to directly. */
    [[nodiscard]] disk_type_& disk() const noexcept { return *disk_; }

    /** Closes every run file, which a destructor does for a caller that did not. */
    status_t unmount() noexcept {
        if (!disk_) return {};
        status_t first;
        for (run_file_t& file : run_files_.records())
            if (file.medium.held())
                if (status_t const closed = disk_->unregister_medium(file.medium); closed.failed() && first.succeeded())
                    first = closed;
        run_files_ = run_files_t {};
        disk_ = nullptr;
        lock_ = file_lock_t {};
        return first;
    }

  private:

    /** One run file, registered while it is live. */
    struct run_file_t {
        registered_medium_t medium;
        uuid128_t run;
        std::uint64_t bytes = 0;
        std::uint32_t generation = 0;
        std::uint16_t data_directory = 0;
    };

    /** One data directory, with its quota and the bytes this database's files take there. */
    struct directory_t {
        string_t path;
        std::uint64_t quota = 0;
        std::uint64_t used = 0;
    };

    using run_files_t = st::basic_growing_pool<run_file_t, std::uint32_t, allocator_type_>;

    template <typename integer_type_>
    static constexpr std::size_t limit_of_() noexcept {
        return (std::numeric_limits<integer_type_>::max)();
    }

    static typename run_files_t::handle_t handle_of_(placement_handle_t handle) noexcept {
        return typename run_files_t::handle_t {handle.index, handle.generation};
    }

    basic_directory_volume(disk_type_& disk, allocator_type_ const& allocator) noexcept
        : disk_(&disk), allocator_(allocator) {}

    /** Bytes directory @p index may still take: its quota less its runs, or its filesystem's
     *  free bytes less the runs created there since the last publication, which their sparse
     *  files have not claimed yet. */
    [[nodiscard]] std::uint64_t free_bytes_(std::size_t index) const noexcept {
        directory_t const& directory = directories_[index];
        if (directory.quota != 0) return directory.quota > directory.used ? directory.quota - directory.used : 0;
        expected<std::uint64_t> const free = free_bytes_of(directory.path.c_str());
        if (!free) return 0;
        std::uint64_t claimed = 0;
        for (placement_handle_t const& handle : unflushed_)
            if (run_file_t const* const file = run_files_.at(handle_of_(handle)); file && file->data_directory == index)
                claimed += file->bytes;
        return *free > claimed ? *free - claimed : 0;
    }

    status_t manifest_path_(std::span<char> into) const noexcept {
        char name[path_max_bytes_k];
        std::size_t const length = manifest_.database.size();
        constexpr std::string_view suffix = ".ustore.json";
        if (length + suffix.size() + 1 > sizeof(name))
            return {status_code_t::invalid_argument_k, "A database name outgrew the platform's limit"};
        std::memcpy(name, manifest_.database.data(), length);
        std::memcpy(name + length, suffix.data(), suffix.size());
        name[length + suffix.size()] = '\0';
        return join_path(directories_[0].path.c_str(), name, into);
    }

    status_t run_path_(std::uint16_t directory, uuid128_t const& run, std::span<char> into) const noexcept {
        char name[path_max_bytes_k];
        if (status_t const named = manifest_.file_name(run, name); named.failed()) return named;
        return join_path(directories_[directory].path.c_str(), name, into);
    }

    expected<medium_extent_t> register_run_(std::uint16_t directory, uuid128_t const& run, std::uint64_t length,
                                            medium_access_t access, std::uint64_t bytes) noexcept {
        std::uint32_t const index = run_files_.take();
        if (index == run_files_t::absent_k)
            return status_t {status_code_t::capacity_exhausted_k, "Every run file this volume tracks is taken"};
        char path[path_max_bytes_k];
        if (status_t const named = run_path_(directory, run, path); named.failed()) {
            run_files_.vacate(index);
            return named;
        }
        medium_options_t request;
        request.name = path;
        request.length = length;
        request.access = access;
        auto [registered, register_status] = disk_->register_medium(request);
        if (register_status.failed()) {
            run_files_.vacate(index);
            return register_status;
        }
        run_file_t& file = run_files_[index];
        file.medium = std::move(registered);
        file.run = run;
        file.bytes = bytes;
        file.data_directory = directory;
        directories_[directory].used += bytes;

        medium_extent_t placed;
        placed.medium = file.medium.reference();
        placed.extent = extent_t {0, bytes};
        placed.data_directory = directory;
        placed.placement = placement_handle_t {index, file.generation};
        return placed;
    }

    /** Unlinks every run file of this database no manifest entry names, and a manifest copy
     *  a crash left. */
    status_t sweep_leftovers_() noexcept {
        st::vector<uuid128_t> named;
        if (named.reserve(manifest_.files.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to list the runs the manifest names"};
        for (manifest_file_t const& file : manifest_.files) [[maybe_unused]]
            st::status_t const placed = named.push_back(st::assume_reserved, uuid128_t {file.run});
        utilities::radix_sort(
            std::span<uuid128_t> {named}, uuid128_t::size_bytes_k,
            [](uuid128_t const& run, std::size_t digit) noexcept -> std::size_t { return run.bytes[digit]; },
            [](uuid128_t const& first, uuid128_t const& second) noexcept { return first < second; });

        st::vector<uuid128_t> leftovers;
        for (std::size_t directory = 0; directory != directories_.size(); ++directory) {
            leftovers.clear();
            status_t listed;
            status_t const walked = list_directory(directories_[directory].path.c_str(), [&](std::string_view name) {
                uuid128_t const run = manifest_.run_of(name);
                if (!run.is_valid() || std::binary_search(named.begin(), named.end(), run)) return;
                if (leftovers.push_back(uuid128_t {run}) != st::status_t::success_k)
                    listed = {status_code_t::out_of_memory_k, "No room to list the leftover run files"};
            });
            if (walked.failed()) return walked;
            if (listed.failed()) return listed;
            char path[path_max_bytes_k];
            for (uuid128_t const& run : leftovers)
                if (status_t const named_path = run_path_(static_cast<std::uint16_t>(directory), run, path);
                    named_path.failed() || remove_file(path).failed())
                    return {status_code_t::io_k, "A leftover run file would not be removed"};
        }
        char path[path_max_bytes_k], temporary[path_max_bytes_k];
        if (status_t const named_path = manifest_path_(path); named_path.failed()) return named_path;
        if (temporary_path_of(path, temporary).failed()) return {};
        return remove_file(temporary);
    }

    disk_type_* disk_ = nullptr;
    allocator_type_ allocator_;
    st::vector<directory_t> directories_;
    run_files_t run_files_;

    /** Run files written since the last publication, which it flushes before it commits. */
    st::vector<placement_handle_t> unflushed_;

    /** Run files the next publication that succeeds gives back, which the manifest on disk
     *  may still name. */
    st::vector<medium_extent_t> unpublished_;

    /** The manifest the mount read, whose generation counts every publication since. */
    manifest_t manifest_;

    /** Held on the manifest's directory for the life of the mount, so no second mount sweeps
     *  this one's runs. */
    file_lock_t lock_;
};

#pragma endregion Directory Volume

#pragma region Slotted Volume

/**
 *  @brief Runs inside 1 GB slots of raw block devices or SPDK namespaces, named by a manifest kept
 *      in the slot at data directory 0's base.
 *
 *  The manifest slot keeps two JSON regions. A publication writes the owner blocks of every slot
 *  taken or freed, writes the JSON into the idle region and flushes every medium, then rewrites the
 *  manifest slot's owner block to point at that region and flushes again. A crash before the second
 *  flush leaves the old generation in force.
 */
template <typename disk_type_, typename waiter_type_, typename allocator_type_>
    requires ring_concept<disk_type_>
class basic_slotted_volume {
  public:

    using disk_t = disk_type_;
    using allocator_t = allocator_type_;
    using slot_allocator_t = basic_slot_allocator<allocator_type_>;

    /** Bytes of each of the manifest slot's two JSON regions. */
    static constexpr std::uint64_t manifest_region_bytes_k = round_down_to_multiple(
        (slot_bytes_k - slot_payload_offset_k) / 2, portable_alignment_k);

    basic_slotted_volume() noexcept = default;
    basic_slotted_volume(basic_slotted_volume const&) = delete;
    basic_slotted_volume& operator=(basic_slotted_volume const&) = delete;

    basic_slotted_volume(basic_slotted_volume&& other) noexcept
        : disk_(std::exchange(other.disk_, nullptr)), allocator_(std::move(other.allocator_)),
          media_(std::move(other.media_)), manifest_(std::move(other.manifest_)),
          manifest_owner_(other.manifest_owner_), unpublished_(std::move(other.unpublished_)) {}

    basic_slotted_volume& operator=(basic_slotted_volume&& other) noexcept {
        if (this == &other) return *this;
        [[maybe_unused]] status_t const released = unmount();
        disk_ = std::exchange(other.disk_, nullptr);
        allocator_ = std::move(other.allocator_);
        media_ = std::move(other.media_);
        manifest_ = std::move(other.manifest_);
        manifest_owner_ = other.manifest_owner_;
        unpublished_ = std::move(other.unpublished_);
        return *this;
    }

    ~basic_slotted_volume() noexcept { [[maybe_unused]] status_t const released = unmount(); }

    /**
     *  @brief Opens every raw medium of @p config, reads one logical block per slot, and rebuilds
     *      the slot pools.
     *
     *  The manifest slot is read first: a zero block makes a fresh database, claimed at once. Every
     *  other slot this database holds that the manifest does not name is a crash leftover, zeroed
     *  and freed; zero blocks are freed in descending order so later takes come out lowest first;
     *  foreign slots stay taken.
     *
     *  @return The volume, @c already_exists_k when the base slot belongs to something else, or
     *      @c malformed_k for media listed in another order than the database was created with.
     */
    static expected<basic_slotted_volume> mount(disk_type_& disk, io_channel_t& channel, volume_config_t const& config,
                                                allocator_type_ allocator) noexcept {
        if (config.data_directories.empty() ||
            config.data_directories.size() > (std::numeric_limits<std::uint16_t>::max)())
            return status_t {status_code_t::invalid_argument_k, "A volume spans one to 65,535 data directories"};
        basic_slotted_volume volume {disk, allocator};
        if (volume.media_.reserve(config.data_directories.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room for the raw media"};
        for (std::size_t index = 0; index != config.data_directories.size(); ++index)
            if (status_t const opened = volume.open_medium_(config.data_directories[index],
                                                            static_cast<std::uint16_t>(index));
                opened.failed())
                return opened;
        if (volume.media_[0].slots.size() == 0)
            return status_t {status_code_t::capacity_exhausted_k, "The first data directory holds no slot"};

        if (status_t const claimed = volume.claim_manifest_slot_(channel, config.database); claimed.failed())
            return claimed;
        for (manifest_file_t const& file : volume.manifest_.files)
            if (status_t const credited = volume.credit_(file); credited.failed()) return credited;
        for (std::size_t index = 0; index != volume.media_.size(); ++index)
            if (status_t const scanned = volume.scan_slots_(channel, index); scanned.failed()) return scanned;
        return volume;
    }

    /** The manifest the mount read, or an empty one naming the database's fresh ID. */
    [[nodiscard]] manifest_t const& manifest() const noexcept { return manifest_; }

    /** How many manifests have been published, whose successor the next publication prints. */
    [[nodiscard]] std::uint64_t generation() const noexcept { return manifest_.generation; }

    /** How the manifests of this volume name their files. */
    [[nodiscard]] static constexpr manifest_placement_t placement() noexcept { return manifest_placement_t::offsets_k; }

    /** The extent @p file names, which the mount has already credited to its slot. */
    expected<medium_extent_t> open(manifest_file_t const& file) noexcept {
        raw_medium_t const& medium = media_[file.medium];
        std::uint32_t const index = static_cast<std::uint32_t>((file.offset - medium.base) / slot_bytes_k);
        medium_extent_t placed;
        placed.medium = medium.medium.reference();
        placed.extent = extent_t {file.offset, round_up_to_multiple(file.bytes, portable_alignment_k)};
        placed.data_directory = file.medium;
        placed.placement = medium.slots.handle_of(index);
        return placed;
    }

    /** Places @p bytes on the medium with the most free bytes, packing a short run into the
     *  open packed slot. */
    expected<medium_extent_t> allocate([[maybe_unused]] uuid128_t const& run, std::uint64_t bytes) noexcept {
        std::uint64_t const rounded = round_up_to_multiple(bytes, portable_alignment_k);
        std::size_t chosen = media_.size();
        std::uint64_t chosen_free = 0;
        for (std::size_t index = 0; index != media_.size(); ++index) {
            std::uint64_t const free = media_[index].slots.free_bytes();
            if (free < rounded || (chosen != media_.size() && free <= chosen_free)) continue;
            chosen = index;
            chosen_free = free;
        }
        if (chosen == media_.size())
            return status_t {status_code_t::capacity_exhausted_k, "No raw medium has room for that run"};
        raw_medium_t& medium = media_[chosen];
        auto [placed, allocate_status] = medium.slots.allocate(rounded);
        if (allocate_status.failed()) return allocate_status;
        placed.medium = medium.medium.reference();
        placed.extent.offset += medium.base;
        return placed;
    }

    /** Gives @p extent back once the manifest that stopped naming it is durable; the next
     *  publication zeroes the run's first block, so a slot walk never mistakes it for a live run,
     *  and a slot left empty is zeroed and deallocated. */
    void deallocate(medium_extent_t const& extent) noexcept {
        if (extent.data_directory >= media_.size()) return;
        raw_medium_t& medium = media_[extent.data_directory];
        // A run left unzeroed is a run a recovery may take back, never a run the manifest names.
        [[maybe_unused]] st::status_t const remembered = medium.dropped.push_back(medium_extent_t {extent});
        medium.slots.deallocate(extent);
    }

    /** Gives @p extent back after the next publication that succeeds, for a run the manifest on
     *  the medium may still name, as one whose publication failed past its owner block does. */
    void deallocate_after_publication(medium_extent_t const& extent) noexcept {
        // Without room the slot stays taken until the next mount scans it, which only costs
        // its bytes meanwhile.
        [[maybe_unused]] st::status_t const remembered = unpublished_.push_back(medium_extent_t {extent});
    }

    /** Writes the owner blocks awaiting a write, @p json into the idle region, flushes every
     *  medium, then points the manifest slot's owner block at the new copy and flushes again. */
    status_t publish_manifest(io_channel_t& channel, std::span<char const> json) noexcept {
        if (json.size() > manifest_region_bytes_k)
            return {status_code_t::capacity_exhausted_k, "The manifest outgrew its region of the manifest slot"};
        for (std::size_t index = 0; index != media_.size(); ++index)
            if (status_t const written = write_touched_(channel, index); written.failed()) return written;

        raw_medium_t& first = media_[0];
        std::uint64_t const regions = first.base + slot_payload_offset_k;
        std::uint64_t const idle = manifest_owner_.manifest_offset == regions ? regions + manifest_region_bytes_k
                                                                              : regions;
        auto [writer_channel, writer_channel_status] = channel.acquire();
        if (writer_channel_status.failed()) return writer_channel_status;
        basic_write_pipeline<disk_type_, waiter_type_> writer {*disk_, std::move(writer_channel),
                                                               first.medium.reference()};
        if (status_t const written = write_bytes_(writer, idle, std::as_bytes(json), first.block_bytes);
            written.failed())
            return written;
        if (status_t const durable = writer.make_durable(); durable.failed()) return durable;

        slot_owner_block_t committed = manifest_owner_;
        committed.manifest_generation = manifest_.generation + 1;
        committed.manifest_offset = idle;
        committed.manifest_bytes = json.size();
        if (status_t const written = write_owner_block_(writer, first, first.base, &committed); written.failed())
            return written;
        if (status_t const durable = writer.make_durable(); durable.failed()) return durable;
        manifest_owner_ = committed;
        ++manifest_.generation;
        for (medium_extent_t const& extent : unpublished_) deallocate(extent);
        unpublished_.clear();
        return {};
    }

    /** Answers @c unsupported_k, as a run on raw media is no file an archive could link. */
    status_t file_path(std::uint16_t, uuid128_t const&, std::span<char>) const noexcept {
        return {status_code_t::unsupported_k, "A run on raw media is no file"};
    }

    /** Runs a database on raw media may hold, which the run pools above it are sized by. */
    [[nodiscard]] std::size_t runs_capacity() const noexcept { return (std::numeric_limits<std::uint16_t>::max)() - 1; }

    /** Bytes every raw medium still offers. */
    [[nodiscard]] std::uint64_t free_bytes() const noexcept {
        std::uint64_t free = 0;
        for (raw_medium_t const& medium : media_) free += medium.slots.free_bytes();
        return free;
    }

    /** The transport every read and write of a run goes to directly. */
    [[nodiscard]] disk_type_& disk() const noexcept { return *disk_; }

    /** Closes every raw medium, which a destructor does for a caller that did not. */
    status_t unmount() noexcept {
        if (!disk_) return {};
        status_t first;
        for (raw_medium_t& medium : media_)
            if (medium.medium.held())
                if (status_t const closed = disk_->unregister_medium(medium.medium);
                    closed.failed() && first.succeeded())
                    first = closed;
        media_.clear();
        disk_ = nullptr;
        return first;
    }

  private:

    /** One raw medium: its registration, where its first slot begins, its logical block,
     *  and its slots. */
    struct raw_medium_t {
        file_lock_t claims;
        registered_medium_t medium;
        std::uint64_t base = 0;
        std::uint64_t block_bytes = 0;
        slot_allocator_t slots;

        /** Runs given back since the last publication, whose first pages it zeroes. */
        st::vector<medium_extent_t> dropped;
    };

    /** What a mount finds in one owner block. */
    enum class owner_t : std::uint8_t {
        free_k,
        ours_k,
        misplaced_k,
        foreign_k,
    };

    basic_slotted_volume(disk_type_& disk, allocator_type_ const& allocator) noexcept
        : disk_(&disk), allocator_(allocator) {}

    status_t open_medium_(data_address_t const& address, std::uint16_t index) noexcept {
        if (address.kind != data_address_kind_t::raw_k && address.kind != data_address_kind_t::spdk_k)
            return {status_code_t::invalid_argument_k, "A slotted volume spans raw media alone"};
        char path[path_max_bytes_k];
        file_lock_t claims;
        if (address.kind == data_address_kind_t::spdk_k) {
            // The disk names a namespace by its number and lets one mount register it at a time.
            [[maybe_unused]] std::to_chars_result const written = std::to_chars(path, path + sizeof(path) - 1,
                                                                                address.namespace_id);
            *written.ptr = '\0';
        }
        else {
            if (address.location.size() >= sizeof(path))
                return {status_code_t::invalid_argument_k, "A raw medium's path outgrew the platform's limit"};
            std::memcpy(path, address.location.data(), address.location.size());
            path[address.location.size()] = '\0';
            // Slot claims of two processes would race on the owner blocks, so one mount holds the
            // medium at a time.
            auto [locked, lock_status] = file_lock_t::acquire(path);
            if (lock_status.failed()) return lock_status;
            claims = std::move(locked);
        }
        medium_options_t request;
        request.name = path;
        request.access = medium_access_t::read_write_k;
        auto [registered, register_status] = disk_->register_medium(request);
        if (register_status.failed()) return register_status;

        raw_medium_t medium;
        medium.claims = std::move(claims);
        medium.medium = std::move(registered);
        medium.base = address.byte_base;
        medium.block_bytes = medium.medium.logical_block_bytes();
        std::uint64_t const capacity = medium.medium.capacity();
        status_t refused;
        if (medium.block_bytes < sizeof(slot_owner_block_t) || medium.block_bytes > slot_payload_offset_k)
            refused = {status_code_t::unsupported_k, "A raw medium's logical block does not hold an owner block"};
        else if (medium.base % medium.block_bytes != 0 || medium.base > capacity)
            refused = {status_code_t::invalid_argument_k, "A raw medium's base is not a block inside it"};
        std::uint64_t usable = capacity - (refused.failed() ? 0 : medium.base);
        if (address.byte_limit != 0 && address.byte_limit < usable) usable = address.byte_limit;
        if (refused.succeeded()) {
            auto [slots, make_status] = slot_allocator_t::make(index, static_cast<std::size_t>(usable / slot_bytes_k),
                                                               allocator_);
            if (make_status.failed()) refused = make_status;
            else medium.slots = std::move(slots);
        }
        if (refused.succeeded() && media_.push_back(std::move(medium)) != st::status_t::success_k)
            refused = {status_code_t::out_of_memory_k, "No room for another raw medium"};
        if (refused.failed() && medium.medium.held()) [[maybe_unused]]
            status_t const closed = disk_->unregister_medium(medium.medium);
        return refused;
    }

    /** Reads the manifest slot's owner block: a zero block becomes this database's, claimed at
     *  once; a block of this layout names the database and the manifest copy in force. */
    status_t claim_manifest_slot_(io_channel_t& channel, string_view_t database) noexcept {
        raw_medium_t& first = media_[0];
        slot_owner_block_t found {};
        // The reader gives the queue pair back before the writer or the manifest load takes it.
        {
            auto [reader_channel, reader_channel_status] = channel.acquire();
            if (reader_channel_status.failed()) return reader_channel_status;
            basic_read_pipeline<disk_type_> reader {*disk_, std::move(reader_channel), first.medium.reference()};
            if (status_t const read = read_owner_blocks_(
                    reader, first, 0, 1,
                    [&](std::uint32_t, slot_owner_block_t const& block) noexcept { found = block; });
                read.failed())
                return read;
        }

        if (is_zero_(found)) {
            manifest_.id = uuid_generator();
            if (sz::failed(manifest_.database.assign(database)))
                return {status_code_t::out_of_memory_k, "No room for the database's name"};
            manifest_.placement = manifest_placement_t::offsets_k;
            manifest_owner_ = owner_block_(0);
            auto [writer_channel, writer_channel_status] = channel.acquire();
            if (writer_channel_status.failed()) return writer_channel_status;
            basic_write_pipeline<disk_type_, waiter_type_> writer {*disk_, std::move(writer_channel),
                                                                   first.medium.reference()};
            if (status_t const written = write_owner_block_(writer, first, first.base, &manifest_owner_);
                written.failed())
                return written;
            if (status_t const durable = writer.make_durable(); durable.failed()) return durable;
        }
        else {
            if (found.magic != slot_owner_magic_k || found.format_version != slot_format_version_k)
                return {status_code_t::already_exists_k, "The base slot belongs to something else"};
            if (found.medium != 0)
                return {status_code_t::malformed_k, "The data directories are listed in another order"};
            manifest_owner_ = found;
            manifest_.id = found.database;
            if (found.manifest_bytes != 0)
                if (status_t const loaded = load_manifest_(channel, found); loaded.failed()) return loaded;
        }
        first.slots.credit(0, 0, slot_bytes_k);
        return {};
    }

    /** Reads the manifest copy @p owner points at and parses it. */
    status_t load_manifest_(io_channel_t& channel, slot_owner_block_t const& owner) noexcept {
        raw_medium_t& first = media_[0];
        std::uint64_t const regions = first.base + slot_payload_offset_k;
        if (owner.manifest_bytes > manifest_region_bytes_k ||
            (owner.manifest_offset != regions && owner.manifest_offset != regions + manifest_region_bytes_k))
            return {status_code_t::malformed_k, "The manifest slot points outside its regions"};
        st::vector<char> text;
        std::uint64_t const rounded = round_up_to_multiple(owner.manifest_bytes, first.block_bytes);
        if (text.resize(static_cast<std::size_t>(rounded)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to read the manifest"};
        auto [reader_channel, reader_channel_status] = channel.acquire();
        if (reader_channel_status.failed()) return reader_channel_status;
        basic_read_pipeline<disk_type_> reader {*disk_, std::move(reader_channel), first.medium.reference()};
        std::uint64_t const block_bytes = round_down_to_multiple(channel.requests().buffer_bytes(), first.block_bytes);
        std::uint64_t offered = 0;
        status_t carried;
        auto sink = [&](completed_buffer_t const& completed) noexcept {
            if (status_t const whole = require_completed_in_full(completed); whole.failed()) carried = whole;
            else std::memcpy(text.data() + completed.tag * block_bytes, completed.bytes.data(), completed.bytes.size());
        };
        for (request_tag_t tag = 0; offered != rounded; ++tag) {
            std::uint64_t const chunk = rounded - offered < block_bytes ? rounded - offered : block_bytes;
            while (!reader.can_enqueue())
                if (status_t const polled = reader.poll_one_completion(sink); polled.failed()) return polled;
            if (status_t const queued = reader.enqueue_read(extent_t {owner.manifest_offset + offered, chunk}, tag);
                queued.failed())
                return queued;
            if (status_t const submitted = reader.submit(); submitted.failed()) return submitted;
            offered += chunk;
        }
        if (status_t const polled = reader.poll_all_completions(sink); polled.failed()) return polled;
        if (carried.failed()) return carried;
        if (status_t const parsed = manifest_.parse(std::span<char> {text.data(), owner.manifest_bytes}, allocator_);
            parsed.failed())
            return parsed;
        if (manifest_.id != owner.database || manifest_.generation != owner.manifest_generation ||
            (!manifest_.files.empty() && manifest_.placement != manifest_placement_t::offsets_k))
            return {status_code_t::malformed_k, "The manifest copy disagrees with the slot pointing at it"};
        manifest_.placement = manifest_placement_t::offsets_k;
        return {};
    }

    /** Credits the slot @p file lies in, refusing a file outside every slot. */
    status_t credit_(manifest_file_t const& file) noexcept {
        if (file.medium >= media_.size())
            return {status_code_t::malformed_k, "The manifest names a data directory this mount lacks"};
        raw_medium_t& medium = media_[file.medium];
        std::uint64_t const slots_end = medium.base + medium.slots.size() * slot_bytes_k;
        if (file.offset < medium.base || file.offset > slots_end || file.bytes > slots_end - file.offset)
            return {status_code_t::malformed_k, "The manifest names bytes outside the slots"};
        std::uint64_t const inside = file.offset - medium.base;
        std::uint32_t const index = static_cast<std::uint32_t>(inside / slot_bytes_k);
        std::uint64_t const offset = inside % slot_bytes_k;
        std::uint64_t const bytes = round_up_to_multiple(file.bytes, portable_alignment_k);
        if (offset < slot_payload_offset_k || offset % portable_alignment_k != 0 || bytes > slot_bytes_k - offset)
            return {status_code_t::malformed_k, "The manifest names a run across a slot's edge"};
        medium.slots.credit(index, offset, bytes);
        return {};
    }

    /** Reads every owner block of medium @p index, then frees zero and leftover slots and
     *  refuses misplaced ones. */
    status_t scan_slots_(io_channel_t& channel, std::size_t index) noexcept {
        raw_medium_t& medium = media_[index];
        std::uint32_t const count = static_cast<std::uint32_t>(medium.slots.size());
        st::vector<owner_t> owners;
        if (owners.resize(count, owner_t::foreign_k) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to classify a medium's slots"};
        std::uint32_t const first_scanned = index == 0 ? 1 : 0;
        // The reader gives the queue pair back before the writer below takes it.
        {
            auto [reader_channel, reader_channel_status] = channel.acquire();
            if (reader_channel_status.failed()) return reader_channel_status;
            basic_read_pipeline<disk_type_> reader {*disk_, std::move(reader_channel), medium.medium.reference()};
            if (status_t const read = read_owner_blocks_(
                    reader, medium, first_scanned, count,
                    [&](std::uint32_t slot, slot_owner_block_t const& block) noexcept {
                        if (is_zero_(block)) owners[slot] = owner_t::free_k;
                        else if (block.magic != slot_owner_magic_k || block.database != manifest_.id)
                            owners[slot] = owner_t::foreign_k;
                        else owners[slot] = block.medium == index ? owner_t::ours_k : owner_t::misplaced_k;
                    });
                read.failed())
                return read;
        }

        auto [writer_channel, writer_channel_status] = channel.acquire();
        if (writer_channel_status.failed()) return writer_channel_status;
        basic_write_pipeline<disk_type_, waiter_type_> writer {*disk_, std::move(writer_channel),
                                                               medium.medium.reference()};
        for (std::uint32_t slot = count; slot != first_scanned; --slot) {
            std::uint32_t const at = slot - 1;
            bool const named = medium.slots.slot(at).live_bytes != 0;
            if (owners[at] == owner_t::misplaced_k)
                return {status_code_t::malformed_k, "The data directories are listed in another order"};
            if (named && owners[at] != owner_t::ours_k)
                return {status_code_t::malformed_k, "The manifest names a slot this database does not hold"};
            if (owners[at] == owner_t::ours_k && !named)
                if (status_t const zeroed = free_on_device_(writer, medium, at); zeroed.failed()) return zeroed;
            if (owners[at] == owner_t::free_k || (owners[at] == owner_t::ours_k && !named)) medium.slots.release(at);
        }
        medium.slots.reopen_packed();
        return writer.make_durable();
    }

    /** Writes the owner block of every slot of medium @p index taken since the last publication,
     *  and zeroes and deallocates every one freed. */
    status_t write_touched_(io_channel_t& channel, std::size_t index) noexcept {
        raw_medium_t& medium = media_[index];
        auto [writer_channel, writer_channel_status] = channel.acquire();
        if (writer_channel_status.failed()) return writer_channel_status;
        basic_write_pipeline<disk_type_, waiter_type_> writer {*disk_, std::move(writer_channel),
                                                               medium.medium.reference()};
        slot_owner_block_t const owner = owner_block_(static_cast<std::uint32_t>(index));
        // A slot vacated since a run was dropped has its owner block zeroed below, and may hold a
        // newer run already.
        for (medium_extent_t const& dropped : medium.dropped)
            if (medium.slots.holds(dropped.placement))
                if (status_t const zeroed = zero_first_page_(writer, dropped.extent.offset); zeroed.failed())
                    return zeroed;
        medium.dropped.clear();
        if (status_t const drained = medium.slots.drain_touched([&](std::uint32_t slot, slot_change_t change) noexcept {
                return change == slot_change_t::taken_k
                           ? write_owner_block_(writer, medium, slot_base_(medium, slot), &owner)
                           : free_on_device_(writer, medium, slot);
            });
            drained.failed())
            return drained;
        return writer.make_durable();
    }

    /** Zeroes the owner block of @p slot and deallocates its payload, leaving the zeroed
     *  block in place. */
    status_t free_on_device_(basic_write_pipeline<disk_type_, waiter_type_>& writer, raw_medium_t const& medium,
                             std::uint32_t slot) noexcept {
        std::uint64_t const base = slot_base_(medium, slot);
        if (status_t const zeroed = write_owner_block_(writer, medium, base, nullptr); zeroed.failed()) return zeroed;
        return writer.enqueue_deallocate(extent_t {base + slot_payload_offset_k, slot_bytes_k - slot_payload_offset_k});
    }

    /** Stages a page of zeros over the first page of the run at @p offset, which
     *  its header occupies. */
    static status_t zero_first_page_(basic_write_pipeline<disk_type_, waiter_type_>& writer,
                                     std::uint64_t offset) noexcept {
        auto [block, acquire_status] = writer.acquire_buffer_or_poll();
        if (acquire_status.failed()) return acquire_status;
        std::memset(block.bytes().data(), 0, static_cast<std::size_t>(portable_alignment_k));
        if (status_t const queued = writer.enqueue_write(std::move(block), extent_t {offset, portable_alignment_k});
            queued.failed())
            return queued;
        return writer.submit();
    }

    /** Stages @p owner, or zeroes when null, over the first logical block at @p offset. */
    static status_t write_owner_block_(basic_write_pipeline<disk_type_, waiter_type_>& writer,
                                       raw_medium_t const& medium, std::uint64_t offset,
                                       slot_owner_block_t const* owner) noexcept {
        auto [block, acquire_status] = writer.acquire_buffer_or_poll();
        if (acquire_status.failed()) return acquire_status;
        std::span<byte_t> const bytes = block.bytes();
        std::memset(bytes.data(), 0, static_cast<std::size_t>(medium.block_bytes));
        if (owner) std::memcpy(bytes.data(), owner, sizeof(slot_owner_block_t));
        if (status_t const queued = writer.enqueue_write(std::move(block), extent_t {offset, medium.block_bytes});
            queued.failed())
            return queued;
        return writer.submit();
    }

    /** Stages @p bytes zero-padded to @p block_bytes at @p offset, one channel block at a time. */
    static status_t write_bytes_(basic_write_pipeline<disk_type_, waiter_type_>& writer, std::uint64_t offset,
                                 std::span<byte_t const> bytes, std::uint64_t block_bytes) noexcept {
        std::uint64_t const total = round_up_to_multiple<std::uint64_t>(bytes.size(), block_bytes);
        for (std::uint64_t written = 0; written != total;) {
            auto [block, acquire_status] = writer.acquire_buffer_or_poll();
            if (acquire_status.failed()) return acquire_status;
            std::uint64_t const room = round_down_to_multiple<std::uint64_t>(block.size(), block_bytes);
            std::uint64_t const chunk = total - written < room ? total - written : room;
            std::span<byte_t> const target = block.bytes();
            std::uint64_t const copied = written < bytes.size()
                                             ? (bytes.size() - written < chunk ? bytes.size() - written : chunk)
                                             : 0;
            std::memset(target.data(), 0, static_cast<std::size_t>(chunk));
            if (copied) std::memcpy(target.data(), bytes.data() + written, static_cast<std::size_t>(copied));
            if (status_t const queued = writer.enqueue_write(std::move(block), extent_t {offset + written, chunk});
                queued.failed())
                return queued;
            if (status_t const submitted = writer.submit(); submitted.failed()) return submitted;
            written += chunk;
        }
        return {};
    }

    /** Reads the owner blocks of slots @p begin up to @p end of @p medium, handing each
     *  to @p visitor. */
    template <typename visitor_type_>
    static status_t read_owner_blocks_(basic_read_pipeline<disk_type_>& reader, raw_medium_t const& medium,
                                       std::uint32_t begin, std::uint32_t end, visitor_type_&& visitor) noexcept {
        status_t carried;
        auto sink = [&](completed_buffer_t const& completed) noexcept {
            if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
                carried = whole;
                return;
            }
            slot_owner_block_t block {};
            std::memcpy(&block, completed.bytes.data(), sizeof(block));
            visitor(static_cast<std::uint32_t>(completed.tag), block);
        };
        for (std::uint32_t slot = begin; slot != end; ++slot) {
            while (!reader.can_enqueue())
                if (status_t const polled = reader.poll_one_completion(sink); polled.failed()) return polled;
            if (status_t const queued = reader.enqueue_read(extent_t {slot_base_(medium, slot), medium.block_bytes},
                                                            slot);
                queued.failed())
                return queued;
            if (status_t const submitted = reader.submit(); submitted.failed()) return submitted;
        }
        if (status_t const polled = reader.poll_all_completions(sink); polled.failed()) return polled;
        return carried;
    }

    [[nodiscard]] static std::uint64_t slot_base_(raw_medium_t const& medium, std::uint32_t slot) noexcept {
        return medium.base + std::uint64_t {slot} * slot_bytes_k;
    }

    [[nodiscard]] static bool is_zero_(slot_owner_block_t const& block) noexcept {
        slot_owner_block_t const zero {};
        return std::memcmp(&block, &zero, sizeof(block)) == 0;
    }

    /** The owner block every slot of data directory @p medium carries, without
     *  the manifest fields. */
    [[nodiscard]] slot_owner_block_t owner_block_(std::uint32_t medium) const noexcept {
        slot_owner_block_t owner {};
        owner.magic = slot_owner_magic_k;
        owner.format_version = slot_format_version_k;
        owner.medium = medium;
        owner.database = manifest_.id;
        return owner;
    }

    disk_type_* disk_ = nullptr;
    allocator_type_ allocator_;
    st::vector<raw_medium_t> media_;

    /** The manifest the mount read, whose generation counts every publication since. */
    manifest_t manifest_;

    /** The manifest slot's owner block as last committed. */
    slot_owner_block_t manifest_owner_ {};

    /** Runs the next publication that succeeds gives back, which the manifest on the media
     *  may still name. */
    st::vector<medium_extent_t> unpublished_;
};

#pragma endregion Slotted Volume

#pragma region Any Volume

/** The volume a mount opened: a directory volume when its first data directory is a
 *  directory, a slotted volume when it is a raw medium; every call is one @c visit, paid per
 *  run rather than per request. */
template <typename disk_type_, typename waiter_type_, typename allocator_type_>
    requires ring_concept<disk_type_>
class any_volume {
  public:

    using disk_t = disk_type_;
    using directory_volume_t = basic_directory_volume<disk_type_, waiter_type_, allocator_type_>;
    using slotted_volume_t = basic_slotted_volume<disk_type_, waiter_type_, allocator_type_>;

    any_volume() noexcept = default;
    any_volume(any_volume&&) noexcept = default;
    any_volume& operator=(any_volume&&) noexcept = default;

    /** Mounts the volume @p config's first data directory calls for. */
    static expected<any_volume> mount(disk_type_& disk, io_channel_t& channel, volume_config_t const& config,
                                      allocator_type_ allocator) noexcept {
        if (config.data_directories.empty())
            return status_t {status_code_t::invalid_argument_k, "A volume spans at least one data directory"};
        any_volume volume;
        volume.disk_ = &disk;
        if (config.data_directories[0].kind == data_address_kind_t::directory_k) {
            auto [opened, mount_status] = directory_volume_t::mount(disk, channel, config, std::move(allocator));
            if (mount_status.failed()) return mount_status;
            volume.held_.template emplace<directory_volume_t>(std::move(opened));
        }
        else {
            auto [opened, mount_status] = slotted_volume_t::mount(disk, channel, config, std::move(allocator));
            if (mount_status.failed()) return mount_status;
            volume.held_.template emplace<slotted_volume_t>(std::move(opened));
        }
        return volume;
    }

    [[nodiscard]] manifest_t const& manifest() const noexcept {
        return visit_(*this, [](auto const& held) noexcept -> manifest_t const& { return held.manifest(); });
    }

    [[nodiscard]] std::uint64_t generation() const noexcept { return manifest().generation; }

    [[nodiscard]] manifest_placement_t placement() const noexcept {
        return std::holds_alternative<slotted_volume_t>(held_) ? manifest_placement_t::offsets_k
                                                               : manifest_placement_t::paths_k;
    }

    expected<medium_extent_t> open(manifest_file_t const& file) noexcept {
        return visit_(*this, [&](auto& held) noexcept { return held.open(file); });
    }

    expected<medium_extent_t> allocate(uuid128_t const& run, std::uint64_t bytes) noexcept {
        return visit_(*this, [&](auto& held) noexcept { return held.allocate(run, bytes); });
    }

    void deallocate(medium_extent_t const& extent) noexcept {
        visit_(*this, [&](auto& held) noexcept { held.deallocate(extent); });
    }

    void deallocate_after_publication(medium_extent_t const& extent) noexcept {
        visit_(*this, [&](auto& held) noexcept { held.deallocate_after_publication(extent); });
    }

    status_t publish_manifest(io_channel_t& channel, std::span<char const> json) noexcept {
        return visit_(*this, [&](auto& held) noexcept { return held.publish_manifest(channel, json); });
    }

    [[nodiscard]] std::size_t runs_capacity() const noexcept {
        return visit_(*this, [](auto const& held) noexcept { return held.runs_capacity(); });
    }

    status_t file_path(std::uint16_t medium, uuid128_t const& run, std::span<char> into) const noexcept {
        return visit_(*this, [&](auto const& held) noexcept { return held.file_path(medium, run, into); });
    }

    [[nodiscard]] std::uint64_t free_bytes() const noexcept {
        return visit_(*this, [](auto const& held) noexcept { return held.free_bytes(); });
    }

    [[nodiscard]] disk_type_& disk() const noexcept { return *disk_; }

    /** Whether a volume is mounted, which every call but this one and @c unmount assumes. */
    [[nodiscard]] bool mounted() const noexcept { return !std::holds_alternative<std::monostate>(held_); }

    /** Closes every medium the volume registered, and leaves the handle naming none. */
    status_t unmount() noexcept {
        status_t released;
        if (mounted()) released = visit_(*this, [](auto& held) noexcept { return held.unmount(); });
        held_.template emplace<std::monostate>();
        return released;
    }

  private:

    /** Calls @p visitor with the mounted volume of @p self, which must hold one. */
    template <typename self_type_>
    using directory_reference =
        std::conditional_t<std::is_const_v<self_type_>, directory_volume_t const&, directory_volume_t&>;

    template <typename self_type_, typename visitor_type_>
    static std::invoke_result_t<visitor_type_, directory_reference<self_type_>> visit_(
        self_type_& self, visitor_type_&& visitor) noexcept {
        if (auto* const directory = std::get_if<directory_volume_t>(&self.held_)) return visitor(*directory);
        return visitor(*std::get_if<slotted_volume_t>(&self.held_));
    }

    disk_type_* disk_ = nullptr;
    std::variant<std::monostate, directory_volume_t, slotted_volume_t> held_;
};

#pragma endregion Any Volume

} // namespace unum::ustore::utilities
