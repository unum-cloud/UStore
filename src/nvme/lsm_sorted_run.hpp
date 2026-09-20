/**
 *  @file src/nvme/lsm_sorted_run.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief One immutable run of an LSM level: a header, the index of its key slots, and the values they locate.
 *
 *  A run is one blob of one volume and nothing else, so opening it is a name lookup and every read
 *  afterwards is an extent plus a submit. The index is read into memory at open rather than mapped,
 *  because a namespace has no page cache to map through and a mapped read cannot report a short
 *  transfer, which is exactly what a truncated run looks like.
 *
 *  The three sections are disjoint, so the write path needs no ordering between them: it stages the
 *  header, the cells and the values into one reservation, polls them all, flushes once, and only then
 *  asks the volume to record the name.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <array>       // `std::array`
#include <span>        // `std::span`
#include <type_traits> // `std::remove_reference_t`
#include <utility>     // `std::declval`, `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/buffer_pool.hpp"      // `pooled_buffer_t`
#include "nvme/catalog.hpp"          // `blob_name_t`, `blob_kind_t`, `magic_of`
#include "nvme/extent.hpp"           // `extent_t`, `rounded_up_to`, `portable_alignment_k`
#include "nvme/io_channel.hpp"       // `io_channel_t`
#include "nvme/io_pipeline.hpp"      // `basic_read_pipeline`, `basic_write_pipeline`, `require_completed_in_full`
#include "nvme/key_slot.hpp"         // `key_slot`
#include "nvme/ring.hpp"             // `read_request`, `poll_completions_at_least`
#include "nvme/sorted_run_index.hpp" // `sorted_run_index_layout_t`, `round_up`
#include "nvme/stored_value.hpp"     // `stored_value_size_t`
#include "nvme/types.hpp"            // `value_layout_t`, `is_inline`, `btree_branching_k`
#include "nvme/uuid.hpp"             // `uuid_t`
#include "nvme/volume.hpp"           // `blob_t`

namespace unum::ustore::nvme {

#pragma region Vocabulary

/** What a lookup found, which separates a missing key from one whose value was erased. */
enum class find_result_t : std::uint8_t {

    /** No key slot of the run carries that key. */
    nothing_k,

    /** A key slot carries that key and erases its value. */
    stored_key_k,

    /** A key slot carries that key and locates its value. */
    success_k,
};

/** The word the first eight bytes of a sorted run carry. */
inline constexpr std::uint64_t sorted_run_magic_k = magic_of("USTORERN");

/** The layout the engine writes, which a run of another version is not opened by. */
inline constexpr std::uint32_t sorted_run_format_version_k = 1;

#pragma pack(push, 1)

/** The first bytes of a sorted run, from which every other offset inside it is derived. */
struct sorted_run_header_t {

    /** @c sorted_run_magic_k, so a run is told apart from any other blob of the volume. */
    std::uint64_t magic = 0;

    /** The layout the rest of the run follows, @c sorted_run_format_version_k for this one. */
    std::uint32_t format_version = 0;

    /** Bytes of one key slot, so a run of another key shape is refused rather than misread. */
    std::uint32_t key_slot_bytes = 0;

    /** Key slots the index holds, padding cells excluded. */
    std::uint64_t key_slots_count = 0;

    /** Bytes of the stored values section, before its alignment padding. */
    std::uint64_t stored_values_bytes = 0;

    /** Largest value the collection that wrote this run admits. */
    std::uint32_t value_max_size = 0;

    /** Cells in one row of the index, which a reader lays the rows out by. */
    std::uint16_t branching = 0;

    /** How the values of this run are stored, which must match the reader's key slot. */
    value_layout_t value_layout = value_layout_t::unknown_k;

    /** The identifier this run is named by, which is also its blob's name. */
    uuid_t content_id;
};

#pragma pack(pop)

/** Where one key slot's value sits inside its volume, as an unbuffered read has to ask for it. */
struct value_window_t {

    /** The volume-absolute range a read covers, empty for a value that needs none. */
    extent_t extent;

    /** Bytes of that range before the value's first byte. */
    std::uint32_t skipped = 0;

    /** Bytes the value itself occupies. */
    std::uint32_t bytes = 0;
};

#pragma endregion Vocabulary

#pragma region Layout

/**
 *  @brief Where the header, the index and the stored values of a sorted run sit inside its blob.
 *
 *  Every offset follows from the key slot count alone, so a header carries the count and never the
 *  offsets, and a reader cannot be told a position its own arithmetic disagrees with.
 */
template <typename key_slot_type_>
class sorted_run_layout {
  public:

    /** The key slot the cells of the index hold. */
    using key_slot_t = key_slot_type_;

    /** How the values of a run of these key slots are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

    /** Whether the values sit in the key slots themselves, so the run carries no values section. */
    static constexpr bool holds_values_inline_k = is_inline(value_layout_k);

    /** Cells in one row of the index. */
    static constexpr std::uint64_t branching_k = btree_branching_k;

    /** Bytes the header occupies, padded out to what an unbuffered read accepts. */
    [[nodiscard]] static constexpr std::uint64_t header_bytes() noexcept {
        return rounded_up_to(sizeof(sorted_run_header_t), portable_alignment_k);
    }

    /** Cells the index of @p key_slots_count key slots holds, padding included. */
    [[nodiscard]] static constexpr std::uint64_t cells_count(std::uint64_t key_slots_count) noexcept {
        return round_up(key_slots_count, branching_k);
    }

    /** Bytes those cells occupy, before the section's alignment padding. */
    [[nodiscard]] static constexpr std::uint64_t index_bytes(std::uint64_t key_slots_count) noexcept {
        return cells_count(key_slots_count) * sizeof(key_slot_t);
    }

    /** Bytes the index section occupies, which one read of it covers whole. */
    [[nodiscard]] static constexpr std::uint64_t index_section_bytes(std::uint64_t key_slots_count) noexcept {
        return rounded_up_to(index_bytes(key_slots_count), portable_alignment_k);
    }

    /** First byte of the index inside the blob. */
    [[nodiscard]] static constexpr std::uint64_t index_offset() noexcept { return header_bytes(); }

    /** First byte of the stored values inside the blob, which every key slot locates against. */
    [[nodiscard]] static constexpr std::uint64_t values_offset(std::uint64_t key_slots_count) noexcept {
        return index_offset() + index_section_bytes(key_slots_count);
    }

    /** Bytes the stored values section occupies, which is none for values held inline. */
    [[nodiscard]] static constexpr std::uint64_t values_section_bytes(std::uint64_t stored_values_bytes) noexcept {
        return holds_values_inline_k ? 0 : rounded_up_to(stored_values_bytes, portable_alignment_k);
    }

    /** Bytes the whole run occupies, which is the length its blob is reserved with. */
    [[nodiscard]] static constexpr std::uint64_t blob_bytes(std::uint64_t key_slots_count,
                                                            std::uint64_t stored_values_bytes) noexcept {
        return values_offset(key_slots_count) + values_section_bytes(stored_values_bytes);
    }
};

#pragma endregion Layout

#pragma region Sorted Run

/**
 *  @brief One immutable run of key slots and the values they locate, held as one blob of one volume.
 *
 *  The run borrows the volume and the blob, and owns only the cells of its index. Every call that
 *  moves bytes takes the channel the calling thread holds, since a queue pair belongs to one thread while it
 *  is held and a run outlives every thread that ever reads it.
 */
template <typename key_slot_type_, typename volume_type_>
class lsm_sorted_run {
  public:

#pragma region Types

    /** The key slot the cells of the index hold. */
    using key_slot_t = key_slot_type_;

    /** The volume the run's blob lives on. */
    using volume_t = volume_type_;

    /** The key the run orders its key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The transport the volume moves bytes through. */
    using disk_t = std::remove_reference_t<decltype(std::declval<volume_type_&>().disk())>;

    /** Where the sections of this run sit inside its blob. */
    using sorted_run_layout_t = sorted_run_layout<key_slot_t>;

    /** The pipeline every read of this run refills. */
    using read_pipeline_t = basic_read_pipeline<disk_t>;

    /** The pipeline the write path stages its three sections through. */
    using write_pipeline_t = basic_write_pipeline<disk_t>;

    /** How the values of this run are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

    /** Whether the values sit in the key slots themselves, so reading one needs no I/O. */
    static constexpr bool holds_values_inline_k = is_inline(value_layout_k);

#pragma endregion Types

#pragma region Construction

    lsm_sorted_run() noexcept = default;
    lsm_sorted_run(lsm_sorted_run const&) = delete;
    lsm_sorted_run& operator=(lsm_sorted_run const&) = delete;

    lsm_sorted_run(lsm_sorted_run&& other) noexcept
        : volume_(std::exchange(other.volume_, nullptr)), blob_(other.blob_), header_(other.header_),
          layout_(other.layout_), cells_(std::move(other.cells_)) {
        other.blob_ = blob_t {};
        other.header_ = sorted_run_header_t {};
    }

    lsm_sorted_run& operator=(lsm_sorted_run&& other) noexcept {
        if (this == &other) return *this;
        volume_ = std::exchange(other.volume_, nullptr);
        blob_ = other.blob_;
        header_ = other.header_;
        layout_ = other.layout_;
        cells_ = std::move(other.cells_);
        other.blob_ = blob_t {};
        other.header_ = sorted_run_header_t {};
        return *this;
    }

#pragma endregion Construction

#pragma region Names

    /** The name a run of @p content_id takes, which borrows @p content_id and never copies it. */
    [[nodiscard]] static blob_name_t name_of(uuid_t const& content_id) noexcept {
        return blob_name_t {reinterpret_cast<byte_t const*>(content_id.bytes), uuid_t::size_bytes_k};
    }

    /** The bytes this run answers to, which are the sixteen of its identifier and never text. */
    [[nodiscard]] blob_name_t name() const noexcept { return name_of(header_.content_id); }

    /** The identifier this run was written under. */
    [[nodiscard]] uuid_t content_id() const noexcept { return header_.content_id; }

#pragma endregion Names

#pragma region Setup and Teardown

    /**
     *  @brief Writes a whole run into a fresh reservation of @p volume and records it under @p content_id.
     *  @param[inout] volume The volume the blob is reserved on, borrowed and outliving the run.
     *  @param[inout] channel The channel the sections are staged on, held by the calling thread.
     *  @param[in] content_id The identifier the run is named by, which must name no blob yet.
     *  @param[in] key_slots The key slots in ascending order of their keys, which the index is laid from.
     *  @param[in] stored_values The values back to back, located from @c values_offset of the layout.
     *  @param[in] value_max_size The largest value the writing collection admits, kept for a reader.
     *  @return The run, opened again from the volume, or the status that stopped the write.
     */
    static expected<lsm_sorted_run> write(volume_type_& volume, io_channel_t& channel, uuid_t const& content_id,
                                          std::span<key_slot_t const> key_slots, std::span<byte_t const> stored_values,
                                          stored_value_size_t value_max_size) noexcept {
        if (!content_id.is_valid())
            return status_t {status_code_t::invalid_argument_k, "A run is named by a valid identifier"};
        if (holds_values_inline_k && !stored_values.empty())
            return status_t {status_code_t::invalid_argument_k, "An inline run carries no stored values"};
        for (std::size_t index = 1; index < key_slots.size(); ++index)
            if (!(key_slots[index - 1].stored_key < key_slots[index].stored_key))
                return status_t {status_code_t::invalid_argument_k, "A run takes its key slots in ascending order"};

        std::uint64_t const block_bytes = rounded_down_to(channel.requests().buffer_bytes(), portable_alignment_k);
        if (block_bytes < sorted_run_layout_t::header_bytes())
            return status_t {status_code_t::capacity_exhausted_k, "A channel's block is shorter than a run's header"};

        sorted_run_header_t header {};
        header.magic = sorted_run_magic_k;
        header.format_version = sorted_run_format_version_k;
        header.key_slot_bytes = static_cast<std::uint32_t>(sizeof(key_slot_t));
        header.key_slots_count = key_slots.size();
        header.stored_values_bytes = stored_values.size();
        header.value_max_size = value_max_size;
        header.branching = static_cast<std::uint16_t>(sorted_run_layout_t::branching_k);
        header.value_layout = value_layout_k;
        header.content_id = content_id;

        std::uint64_t const reserved_bytes = sorted_run_layout_t::blob_bytes(header.key_slots_count,
                                                                             header.stored_values_bytes);
        expected<blob_t> reserved = volume.create(name_of(content_id), blob_kind_t::sorted_run_k, reserved_bytes);
        if (!reserved) return reserved.status();
        blob_t const blob = *reserved;

        if (status_t const written = write_sections_(volume, channel, blob, header, key_slots, stored_values,
                                                     block_bytes);
            written.failed()) {
            [[maybe_unused]] status_t const abandoned = volume.drop(channel, name_of(content_id));
            return written;
        }
        if (status_t const sealed = volume.seal(channel, blob); sealed.failed()) {
            [[maybe_unused]] status_t const abandoned = volume.drop(channel, name_of(content_id));
            return sealed;
        }
        return attach(volume, channel, blob);
    }

    /** Opens the run @p content_id names on @p volume, or answers @c not_found_k when it names none. */
    static expected<lsm_sorted_run> open(volume_type_& volume, io_channel_t& channel,
                                         uuid_t const& content_id) noexcept {
        expected<blob_t> const found = volume.find(name_of(content_id));
        if (!found) return found.status();
        return attach(volume, channel, *found);
    }

    /**
     *  @brief Reads the header and the whole index of @p blob, which is everything a run keeps in memory.
     *  @param[inout] volume The volume the run reads through afterwards, borrowed and outliving it.
     *  @param[inout] channel The channel the header and the index are read on, held by the calling thread.
     *  @param[in] blob The reservation the run occupies, as a lookup or a router handed it over.
     *  @return The run, or @c status_code_t::malformed_k for a blob shorter than the run it claims.
     */
    static expected<lsm_sorted_run> attach(volume_type_& volume, io_channel_t& channel, blob_t const& blob) noexcept {
        lsm_sorted_run run;
        run.volume_ = &volume;
        run.blob_ = blob;
        if (status_t const head = run.read_header_(channel); head.failed()) return head;
        if (status_t const index = run.read_index_(channel); index.failed()) return index;
        return run;
    }

    /** Forgets the run @p content_id names and returns its bytes to @p volume. */
    static status_t drop(volume_type_& volume, io_channel_t& channel, uuid_t const& content_id) noexcept {
        return volume.drop(channel, name_of(content_id));
    }

#pragma endregion Setup and Teardown

#pragma region Lookups

    /** Whether this run holds an index at all, which a default-constructed one does not. */
    [[nodiscard]] bool attached() const noexcept { return volume_ != nullptr; }

    /** Key slots the run holds, padding cells excluded. */
    [[nodiscard]] std::uint64_t population_count() const noexcept { return header_.key_slots_count; }

    /** Bytes the run's blob takes on its volume. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept { return blob_.extent.length; }

    /** The blob the run occupies, which a router or a backup moves by. */
    [[nodiscard]] blob_t const& blob() const noexcept { return blob_; }

    /** How the cells of the index map to sorted positions. */
    [[nodiscard]] sorted_run_index_layout_t const& layout() const noexcept { return layout_; }

    /** The cells of the index in cell order, padding included, which a compaction reads in place. */
    [[nodiscard]] std::span<key_slot_t const> cells() const noexcept { return std::span<key_slot_t const> {cells_}; }

    /** The key slot at sorted @p position, which must be below @c population_count. */
    [[nodiscard]] key_slot_t const& key_slot(std::uint64_t position) const noexcept {
        return cells_[static_cast<std::size_t>(layout_.sorted_to_cell(position))];
    }

    /** The least key the run holds, or the greatest key of the type for an empty run. */
    [[nodiscard]] stored_key_t lower_stored_key() const noexcept {
        return header_.key_slots_count != 0 ? key_slot(0).stored_key : limit<stored_key_t>::max();
    }

    /** The greatest key the run holds, or the least key of the type for an empty run. */
    [[nodiscard]] stored_key_t upper_stored_key() const noexcept {
        return header_.key_slots_count != 0 ? key_slot(header_.key_slots_count - 1).stored_key
                                            : limit<stored_key_t>::min();
    }

    /** Where @p stored_key sits in sorted order, or @c population_count when the run holds no such key. */
    [[nodiscard]] std::uint64_t position_of(stored_key_t stored_key) const noexcept {
        std::uint64_t low = 0;
        std::uint64_t high = header_.key_slots_count;
        while (low != high) {
            std::uint64_t const middle = low + (high - low) / 2;
            if (key_slot(middle).stored_key < stored_key) low = middle + 1;
            else high = middle;
        }
        if (low == header_.key_slots_count) return header_.key_slots_count;
        return key_slot(low).stored_key == stored_key ? low : header_.key_slots_count;
    }

    /**
     *  @brief Whether the run carries @p stored_key, and whether that key slot locates a value.
     *  @param[in] stored_key The key to look for.
     *  @param[out] position Where the key sits, or @c population_count when the run holds no such key.
     */
    [[nodiscard]] find_result_t find(stored_key_t stored_key, std::uint64_t& position) const noexcept {
        position = position_of(stored_key);
        if (position == header_.key_slots_count) return find_result_t::nothing_k;
        return key_slot(position).is_tombstone() ? find_result_t::stored_key_k : find_result_t::success_k;
    }

    /** Calls @p visitor with the position and the key slot of every key, in ascending order of keys. */
    template <typename visitor_type_>
    void for_each_key_slot(visitor_type_&& visitor) const noexcept {
        for (std::uint64_t position = 0; position != header_.key_slots_count; ++position)
            visitor(position, key_slot(position));
    }

    /** Where the value of the key slot at @p position sits, empty for one that needs no read. */
    [[nodiscard]] value_window_t value_window(std::uint64_t position) const noexcept {
        key_slot_t const& slot = key_slot(position);
        value_window_t window;
        if constexpr (holds_values_inline_k) {
            window.bytes = slot.is_tombstone() ? 0 : slot.size;
            return window;
        }
        else {
            std::uint64_t const covered = slot.aligned_buffer_size();
            if (covered == 0) return window;
            window.extent = blob_.slice(slot.aligned_offset_in_file(), covered);
            window.skipped = slot.buffer_padding();
            window.bytes = slot.size;
            return window;
        }
    }

#pragma endregion Lookups

#pragma region Batched Reads

    /**
     *  @brief Reads the values of the key slots at @p positions, refilling the channel as completions retire.
     *  @param[inout] channel The channel the reads are submitted and completed on, held by the calling thread.
     *  @param[in] positions Sorted positions of the key slots whose values are wanted.
     *  @param[in] sink Called as @c sink(request, offset, bytes) while the channel still owns the block.
     *  @return Success, or the first failure any of the blocks reported.
     *
     *  @note A value larger than one block arrives as several calls, each carrying the bytes of the
     *      value before it, and a key slot that locates nothing is reported once with no bytes.
     */
    template <typename sink_type_>
    status_t read_values(io_channel_t& channel, std::span<std::uint64_t const> positions,
                         sink_type_&& sink) const noexcept {
        if constexpr (holds_values_inline_k) {
            for (std::size_t request = 0; request != positions.size(); ++request) {
                key_slot_t const& slot = key_slot(positions[request]);
                std::size_t const bytes = slot.is_tombstone() ? 0 : slot.size;
                sink(request, std::uint64_t {0}, std::span<byte_t const> {slot.data, bytes});
            }
            return {};
        }
        else {
            std::uint64_t const block_bytes = rounded_down_to(channel.requests().buffer_bytes(), portable_alignment_k);
            if (block_bytes == 0)
                return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than one page"};

            std::size_t const depth = channel.queue_pair().depth();
            st::basic_vector<pending_read_t> pending;
            if (pending.resize(depth) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A run could not track its reads in flight"};

            read_pipeline_t reader {volume_->disk(), channel, blob_.medium};
            status_t first;
            auto adapter = [&](completed_buffer_t const& completed) noexcept {
                pending_read_t const& piece = pending[completed.tag % depth];
                if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
                    if (first.succeeded()) first = whole;
                    return;
                }
                sink(piece.request, piece.offset, completed.bytes.subspan(piece.skipped, piece.carried));
            };

            std::size_t request = 0;
            std::uint64_t chunk = 0;
            std::uint32_t tag = 0;
            while (request != positions.size() || reader.outstanding() != 0) {
                while (request != positions.size() && reader.can_enqueue()) {
                    value_window_t const window = value_window(positions[request]);
                    if (window.extent.length == 0) {
                        sink(request, std::uint64_t {0}, std::span<byte_t const> {});
                        ++request;
                        chunk = 0;
                        continue;
                    }
                    std::uint64_t const at = chunk * block_bytes;
                    std::uint64_t const room = window.extent.length - at;
                    std::uint64_t const length = block_bytes < room ? block_bytes : room;
                    pending[tag % depth] = piece_of_(window, request, at, length);
                    if (reader.enqueue_read(extent_t {window.extent.offset + at, length}, tag).failed()) break;
                    ++tag;
                    if (at + length == window.extent.length) {
                        ++request;
                        chunk = 0;
                    }
                    else ++chunk;
                }
                if (status_t const flushed = reader.submit(); flushed.failed()) return flushed;
                expected<std::size_t> const polled = reader.poll_completions(adapter);
                if (!polled) return polled.status();
            }
            return first;
        }
    }

#pragma endregion Batched Reads

  private:

#pragma region Private Types

    /** One block of one value's read, kept until its completion carries the block back. */
    struct pending_read_t {

        /** Which of the caller's requests this block belongs to. */
        std::size_t request = 0;

        /** Bytes of the block before the value's own bytes. */
        std::uint32_t skipped = 0;

        /** Bytes of the value this block carries. */
        std::uint32_t carried = 0;

        /** Bytes of the value that the blocks before this one carried. */
        std::uint64_t offset = 0;
    };

#pragma endregion Private Types

#pragma region Private Methods

    /** The piece of @p window that the block at @p at of @p length carries, for @p request. */
    [[nodiscard]] static pending_read_t piece_of_(value_window_t const& window, std::size_t request, std::uint64_t at,
                                                  std::uint64_t length) noexcept {
        std::uint64_t const wanted_first = window.skipped;
        std::uint64_t const wanted_last = window.skipped + window.bytes;
        std::uint64_t const first = at > wanted_first ? at : wanted_first;
        std::uint64_t const last = at + length < wanted_last ? at + length : wanted_last;
        pending_read_t piece;
        piece.request = request;
        piece.skipped = static_cast<std::uint32_t>(first - at);
        piece.carried = static_cast<std::uint32_t>(last > first ? last - first : 0);
        piece.offset = first - wanted_first;
        return piece;
    }

    /** Fills @p bytes, which starts @p section_offset bytes into the index, with the cells that lie there. */
    static void paint_cells_(std::span<key_slot_t const> key_slots, sorted_run_index_layout_t const& layout,
                             std::uint64_t section_offset, std::span<byte_t> bytes) noexcept {
        std::uint64_t const painted = layout.cells_count() * sizeof(key_slot_t);
        std::uint64_t written = 0;
        while (written != bytes.size() && section_offset + written < painted) {
            std::uint64_t const at = section_offset + written;
            std::uint64_t const inside = at % sizeof(key_slot_t);
            std::uint64_t const sorted = layout.cell_to_sorted(at / sizeof(key_slot_t));
            key_slot_t const staged = sorted < key_slots.size() ? key_slots[static_cast<std::size_t>(sorted)]
                                                                : key_slot_t {};
            std::uint64_t const room = sizeof(key_slot_t) - inside;
            std::uint64_t const chunk = room < bytes.size() - written ? room : bytes.size() - written;
            std::memcpy(bytes.data() + written, reinterpret_cast<byte_t const*>(&staged) + inside,
                        static_cast<std::size_t>(chunk));
            written += chunk;
        }
    }

    /** Stages the header, the cells and the values into @p blob, and makes all three durable at once. */
    static status_t write_sections_(volume_type_& volume, io_channel_t& channel, blob_t const& blob,
                                    sorted_run_header_t const& header, std::span<key_slot_t const> key_slots,
                                    std::span<byte_t const> stored_values, std::uint64_t block_bytes) noexcept {
        write_pipeline_t writer {volume.disk(), channel, blob.medium};
        sorted_run_index_layout_t const layout {header.key_slots_count, header.branching};

        std::uint64_t const head_bytes = sorted_run_layout_t::header_bytes();
        expected<pooled_buffer_t> head = writer.acquire_buffer_or_poll();
        if (!head) return head.status();
        std::memset(head->bytes().data(), 0, static_cast<std::size_t>(head_bytes));
        std::memcpy(head->bytes().data(), &header, sizeof(header));
        if (status_t const queued = writer.enqueue_write(std::move(*head), blob.slice(0, head_bytes)); queued.failed())
            return queued;
        if (status_t const flushed = writer.submit(); flushed.failed()) return flushed;

        std::uint64_t const index_section = sorted_run_layout_t::index_section_bytes(header.key_slots_count);
        for (std::uint64_t written = 0; written != index_section;) {
            std::uint64_t const room = index_section - written;
            std::uint64_t const chunk = block_bytes < room ? block_bytes : room;
            expected<pooled_buffer_t> block = writer.acquire_buffer_or_poll();
            if (!block) return block.status();
            std::span<byte_t> const bytes = block->bytes().first(static_cast<std::size_t>(chunk));
            std::memset(bytes.data(), 0, bytes.size());
            paint_cells_(key_slots, layout, written, bytes);
            if (status_t const queued = writer.enqueue_write(
                    std::move(*block), blob.slice(sorted_run_layout_t::index_offset() + written, chunk));
                queued.failed())
                return queued;
            if (status_t const flushed = writer.submit(); flushed.failed()) return flushed;
            written += chunk;
        }

        std::uint64_t const values_section = sorted_run_layout_t::values_section_bytes(header.stored_values_bytes);
        std::uint64_t const values_offset = sorted_run_layout_t::values_offset(header.key_slots_count);
        for (std::uint64_t written = 0; written != values_section;) {
            std::uint64_t const room = values_section - written;
            std::uint64_t const chunk = block_bytes < room ? block_bytes : room;
            expected<pooled_buffer_t> block = writer.acquire_buffer_or_poll();
            if (!block) return block.status();
            std::span<byte_t> const bytes = block->bytes().first(static_cast<std::size_t>(chunk));
            std::uint64_t const left = stored_values.size() > written ? stored_values.size() - written : 0;
            std::uint64_t const carried = left < chunk ? left : chunk;
            if (carried != 0)
                std::memcpy(bytes.data(), stored_values.data() + written, static_cast<std::size_t>(carried));
            std::memset(bytes.data() + carried, 0, static_cast<std::size_t>(chunk - carried));
            if (status_t const queued = writer.enqueue_write(std::move(*block),
                                                             blob.slice(values_offset + written, chunk));
                queued.failed())
                return queued;
            if (status_t const flushed = writer.submit(); flushed.failed()) return flushed;
            written += chunk;
        }

        return writer.make_durable();
    }

    /** Reads the first page of the blob and accepts it only as a run this reader can serve. */
    status_t read_header_(io_channel_t& channel) noexcept {
        std::uint64_t const head_bytes = sorted_run_layout_t::header_bytes();
        if (channel.requests().buffer_bytes() < head_bytes)
            return {status_code_t::capacity_exhausted_k, "A channel's block is shorter than a run's header"};
        extent_t const piece = blob_.slice(0, head_bytes);
        if (piece.length != head_bytes) return {status_code_t::malformed_k, "The run is shorter than its header"};

        read_pipeline_t reader {volume_->disk(), channel, blob_.medium};
        if (status_t const offered = reader.enqueue_read(piece, 0); offered.failed()) return offered;
        if (status_t const flushed = reader.submit(); flushed.failed()) return flushed;

        sorted_run_header_t header {};
        status_t moved;
        auto sink = [&](completed_buffer_t const& completed) noexcept {
            if (status_t const whole = require_completed_in_full(completed); whole.failed()) {
                moved = whole;
                return;
            }
            std::memcpy(&header, completed.bytes.data(), sizeof(header));
        };
        if (status_t const polled = reader.poll_all_completions(sink); polled.failed()) return polled;
        if (moved.failed()) return {status_code_t::malformed_k, "A run's header did not read whole"};

        if (header.magic != sorted_run_magic_k) return {status_code_t::malformed_k, "The blob is no sorted run"};
        if (header.format_version != sorted_run_format_version_k)
            return {status_code_t::unsupported_k, "The run follows another format version"};
        if (header.key_slot_bytes != sizeof(key_slot_t) || header.value_layout != value_layout_k)
            return {status_code_t::malformed_k, "The run holds key slots of another shape"};
        if (header.branching < 2) return {status_code_t::malformed_k, "The run names no index branching"};
        if (sorted_run_layout_t::blob_bytes(header.key_slots_count, header.stored_values_bytes) > blob_.extent.length)
            return {status_code_t::malformed_k, "The run claims more bytes than its blob holds"};

        header_ = header;
        layout_ = sorted_run_index_layout_t {header.key_slots_count, header.branching};
        return {};
    }

    /** Reads the whole index into owned memory, which a short transfer makes a truncated run. */
    status_t read_index_(io_channel_t& channel) noexcept {
        std::uint64_t const cells = sorted_run_layout_t::cells_count(header_.key_slots_count);
        if (cells_.resize(static_cast<std::size_t>(cells)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run could not hold its index"};
        std::uint64_t const section = sorted_run_layout_t::index_section_bytes(header_.key_slots_count);
        if (section == 0) return {};

        expected<pooled_buffer_t> block = channel.streams().acquire();
        if (!block) return block.status();
        std::uint64_t const room = rounded_down_to(block->size(), portable_alignment_k);
        if (room == 0) return {status_code_t::capacity_exhausted_k, "A channel's streaming block holds no index page"};

        std::uint64_t const wanted = sorted_run_layout_t::index_bytes(header_.key_slots_count);
        byte_t* const into = reinterpret_cast<byte_t*>(cells_.data());
        for (std::uint64_t read = 0; read != section;) {
            std::uint64_t const chunk = room < section - read ? room : section - read;
            extent_t const piece = blob_.slice(sorted_run_layout_t::index_offset() + read, chunk);
            if (piece.length != chunk) return {status_code_t::malformed_k, "The run is shorter than its index"};

            std::array<request_t, 1> const batch {
                read_request(blob_.medium, piece, block->reference(), channel.next_token())};
            expected<std::size_t> const taken = volume_->disk().submit(channel.queue_pair(),
                                                                       std::span<request_t const> {batch});
            if (!taken) return taken.status();
            if (*taken != 1) return {status_code_t::input_output_k, "The ring would not take an index read"};
            expected<std::size_t> const completed = poll_completions_at_least(volume_->disk(), channel.queue_pair(),
                                                                              channel.completion_buffer(), 1);
            if (!completed) return completed.status();
            if (*completed == 0) return {status_code_t::input_output_k, "An index read never completed"};
            completion_t const& completion = channel.completion_buffer().front();
            if (!completion.completed_in_full())
                return completion.status.failed()
                           ? completion.status
                           : status_t {status_code_t::malformed_k, "The run is shorter than its index"};

            if (read < wanted) {
                std::uint64_t const usable = wanted - read < chunk ? wanted - read : chunk;
                std::memcpy(into + read, block->bytes().data(), static_cast<std::size_t>(usable));
            }
            read += chunk;
        }
        return {};
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The volume every read of this run goes through, null until the run is attached or once it is moved from. */
    volume_type_* volume_ = nullptr;

    /** The reservation the run occupies, which every extent of it is sliced out of. */
    blob_t blob_;

    /** The header of the run, from which every offset inside the blob follows. */
    sorted_run_header_t header_ {};

    /** How the cells of the index map to sorted positions, rebuilt from the header at open. */
    sorted_run_index_layout_t layout_;

    /** The cells of the index, read whole at open and owned for the life of the run. */
    st::basic_vector<key_slot_t> cells_;

#pragma endregion Private Members
};

#pragma endregion Sorted Run

} // namespace unum::ustore::nvme
