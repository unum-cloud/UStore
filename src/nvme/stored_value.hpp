/**
 *  @file src/nvme/stored_value.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Stored value locations, sizes and metadata shared across the LSM levels.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`
#include <type_traits> // `std::conditional_t`
#include <utility>     // `std::move`
#include <variant>     // `std::variant`, `std::visit`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "nvme/io_types.hpp" // `disk_id_t`, `file_id_t`
#include "nvme/types.hpp"    // `value_layout_t`, `bytes_span_t`, the rounding helpers

namespace unum::ustore::nvme {

/** Where one stored value starts inside its sorted run file. */
using stored_value_offset_t = u32_t;

/** How many bytes one stored value occupies. */
using stored_value_size_t = u32_t;

/** The bytes of one stored value a caller may write into. */
using stored_value_span_t = bytes_span_t;

/** The bytes of one stored value a caller may only read. */
using stored_value_view_t = bytes_view_t;

/** The offset of a key whose value has been erased. */
inline constexpr stored_value_offset_t tombstone_offset_k = std::numeric_limits<stored_value_offset_t>::max();

/** The offset of a key whose value is present and empty. */
inline constexpr stored_value_offset_t empty_stored_value_offset_k = tombstone_offset_k - 1;

/** The level or run number of a location that names none. */
inline constexpr u32_t unknown_run_number_k = std::numeric_limits<u32_t>::max();

/** Whether a span of locations may be written through, or only read. */
enum class locations_mutability_t : bool {

    /** The locations are read-only, as a view of somebody else's array. */
    immutable_k,

    /** The locations may be written through. */
    mutable_k,
};

/** Value layouts whose stored values live inline in key slots rather than in sorted run files. */
template <value_layout_t value_layout_>
concept inline_value_layout_concept = is_inline(value_layout_);

/**
 *  @brief Where the value of one key sits, in the shape the collection's value layout gives it.
 *
 *  The unspecialized template is the common head every layout extends, and reports no value at all,
 *  so a collection whose layout is not yet known carries locations that read as missing.
 */
template <value_layout_t value_layout_ = value_layout_t::unknown_k>
struct stored_value_location {

    /** The layout this location was shaped for. */
    static constexpr value_layout_t value_layout_k = value_layout_;

    static_assert(value_layout_k == value_layout_t::unknown_k, "Every named layout has its own specialization");

    /** Whoever asked for the value, so a completed read finds its caller. */
    u64_t recipient_id = 0;

    /** Which of that caller's reads this location belongs to. */
    u32_t recipient_state_id = 0;

    /** Bytes the value occupies. */
    stored_value_size_t size_bytes = 0;

    /** Where the value starts inside its file. */
    stored_value_offset_t offset = 0;

    /** Whether the location names a value, which an unknown layout never does. */
    explicit operator bool() const noexcept { return false; }

    /** Bytes the value occupies. */
    stored_value_size_t size() const noexcept { return 0; }

    /** Bytes a buffer needs for the value, rounded out to what unbuffered I/O accepts. */
    stored_value_size_t aligned_buffer_size() const noexcept { return 0; }

    /** Where an unbuffered read of the value starts inside the file. */
    stored_value_size_t aligned_offset_in_file() const noexcept { return 0; }

    /** Bytes between the start of the aligned read and the value itself. */
    stored_value_size_t buffer_padding() const noexcept { return 0; }
};

/** An inline value sits in the key slot itself, so its location names the run it was read from. */
template <value_layout_t value_layout_>
    requires inline_value_layout_concept<value_layout_>
struct stored_value_location<value_layout_> : public stored_value_location<value_layout_t::unknown_k> {

    /** The LSM level the key slot came from. */
    u32_t lsm_level_number = unknown_run_number_k;

    /** The sorted run of that level the key slot came from. */
    u32_t sorted_run_number = unknown_run_number_k;

    /** Whether the location names a value, which a zero size does not. */
    explicit operator bool() const noexcept { return size_bytes != 0; }

    /** Bytes the value occupies. */
    stored_value_size_t size() const noexcept { return size_bytes; }

    /** Bytes a buffer needs for the value, which needs no alignment when it is inline. */
    stored_value_size_t aligned_buffer_size() const noexcept { return size_bytes; }
};

/** A fixed-width value sits at one offset of one file, and every value of the collection is as wide. */
template <>
struct stored_value_location<value_layout_t::fixed_width_k> : public stored_value_location<value_layout_t::unknown_k> {

    /** The disk holding the file. */
    disk_id_t disk_id = disk_id_t::unknown_k;

    /** The file holding the value. */
    file_id_t file_id = file_id_t::invalid_k;

    /** Whether the location names a value, which a zero size does not. */
    explicit operator bool() const noexcept { return size_bytes != 0; }

    /** Bytes the value occupies. */
    stored_value_size_t size() const noexcept { return size_bytes; }

    /** Bytes a buffer needs for the value, rounded out to what unbuffered I/O accepts. */
    stored_value_size_t aligned_buffer_size() const noexcept {
        stored_value_size_t const padded_size = stored_value_size_t(size_bytes != 0) * (buffer_padding() + size_bytes);
        return roundup_to_multiple<stored_value_size_t, buffer_alignment_k>(padded_size);
    }

    /** Where an unbuffered read of the value starts inside the file. */
    stored_value_size_t aligned_offset_in_file() const noexcept {
        return rounddown_to_multiple<stored_value_offset_t, buffer_alignment_k>(offset);
    }

    /** Bytes between the start of the aligned read and the value itself. */
    stored_value_size_t buffer_padding() const noexcept { return offset - aligned_offset_in_file(); }
};

/** A variable-width value sits at one offset of one file, and carries its own width. */
template <>
struct stored_value_location<value_layout_t::variable_width_k>
    : public stored_value_location<value_layout_t::unknown_k> {

    /** The disk holding the file. */
    disk_id_t disk_id = disk_id_t::unknown_k;

    /** The file holding the value. */
    file_id_t file_id = file_id_t::invalid_k;

    /** Whether the location names a value, which a tombstone does not. */
    explicit operator bool() const noexcept { return offset != tombstone_offset_k; }

    /** Bytes the value occupies. */
    stored_value_size_t size() const noexcept { return size_bytes; }

    /** Bytes a buffer needs for the value, rounded out to what unbuffered I/O accepts. */
    stored_value_size_t aligned_buffer_size() const noexcept {
        bool const is_nonempty = bool(*this) && offset != empty_stored_value_offset_k;
        stored_value_size_t const padded_size = stored_value_size_t(is_nonempty) * (buffer_padding() + size_bytes);
        return roundup_to_multiple<stored_value_size_t, buffer_alignment_k>(padded_size);
    }

    /** Where an unbuffered read of the value starts inside the file. */
    stored_value_size_t aligned_offset_in_file() const noexcept {
        return rounddown_to_multiple<stored_value_offset_t, buffer_alignment_k>(offset);
    }

    /** Bytes between the start of the aligned read and the value itself. */
    stored_value_size_t buffer_padding() const noexcept { return offset - aligned_offset_in_file(); }
};

/**
 *  @brief A batch of locations of one layout, borrowed from whoever owns the arrays.
 *
 *  The span also borrows the two totals the batch keeps, so recalculating them updates the owner's
 *  copies rather than a duplicate the caller would have to write back.
 */
template <value_layout_t value_layout_, locations_mutability_t mutability_ = locations_mutability_t::mutable_k>
class stored_value_locations_span {
  public:
    using stored_value_location_t = stored_value_location<value_layout_>;

    /** The layout every location in the batch was shaped for. */
    static constexpr value_layout_t value_layout_k = value_layout_;

    /** Whether the batch may be written through. */
    static constexpr locations_mutability_t mutability_k = mutability_;

    /** Whether the values of this layout sit in their key slots. */
    static constexpr bool is_inline_k = is_inline(value_layout_k);

    /** One location, const when the batch is a view. */
    using location_t = std::conditional_t<mutability_ == locations_mutability_t::mutable_k, stored_value_location_t,
                                          stored_value_location_t const>;

    /** One offset into the batch's buffer, const when the batch is a view. */
    using offset_in_buffer_t =
        std::conditional_t<mutability_ == locations_mutability_t::mutable_k, std::size_t, std::size_t const>;

    /** Where every value of the batch sits. */
    std::span<location_t> locations;

    /** Where every value of the batch lands inside the batch's buffer, before its padding. */
    std::span<offset_in_buffer_t> aligned_offsets_in_buffer;

    /** How many of the batch's locations name a value. */
    offset_in_buffer_t& stored_values_found;

    /** Bytes the batch's buffer needs for every value it found. */
    offset_in_buffer_t& aligned_buffer_size_bytes;

    /** Locations in the batch, whether or not they name a value. */
    std::size_t count() const noexcept { return locations.size(); }

    /** Locations of the batch that name a value. */
    std::size_t found_count() const noexcept { return stored_values_found; }

    /** Bytes the batch's buffer needs for every value it found. */
    std::size_t total_size_bytes() const noexcept { return aligned_buffer_size_bytes; }

    /** Whether the location at @p index names a value. */
    bool exists(std::size_t index) const noexcept { return bool(locations[index]); }

    /** Bytes the value at @p index occupies, which must exist. */
    stored_value_size_t size(std::size_t index) const noexcept {
        assert(locations[index] && "Stored value doesn't exist");
        return locations[index].size();
    }

    /** Where the value at @p index lands inside the batch's buffer, which must exist. */
    stored_value_size_t offset_in_buffer(std::size_t index) const noexcept {
        assert(locations[index] && "Stored value doesn't exist");
        return aligned_offsets_in_buffer[index] + locations[index].buffer_padding();
    }

    /** Lays the batch out inside one buffer, and counts the values it found. */
    void recalculate_metadata() noexcept {
        stored_values_found = 0;
        aligned_buffer_size_bytes = 0;
        for (std::size_t index = 0; index != locations.size(); ++index) {
            location_t const& location = locations[index];
            stored_values_found += std::size_t(bool(location));
            aligned_offsets_in_buffer[index] = aligned_buffer_size_bytes;
            aligned_buffer_size_bytes += location.aligned_buffer_size();
        }
    }
};

/** A read-only batch of locations of one layout. */
template <value_layout_t value_layout_>
using stored_value_locations_view = stored_value_locations_span<value_layout_, locations_mutability_t::immutable_k>;

/**
 *  @brief A batch of locations of one layout, with the arrays it owns.
 *
 *  Both arrays are sized once by the factory, so no member of the batch allocates afterwards.
 */
template <value_layout_t value_layout_, template <typename> typename allocator_ = heap_allocator>
class stored_value_locations {
  public:
    using stored_value_location_t = stored_value_location<value_layout_>;
    using stored_value_locations_span_t = stored_value_locations_span<value_layout_>;
    using stored_value_locations_view_t = stored_value_locations_view<value_layout_>;

    /** The layout every location in the batch was shaped for. */
    static constexpr value_layout_t value_layout_k = value_layout_;

    /** Whether the values of this layout sit in their key slots. */
    static constexpr bool is_inline_k = is_inline(value_layout_k);

    /** Where every value of the batch sits. */
    st::basic_vector<stored_value_location_t, allocator_<stored_value_location_t>> locations;

    /** Where every value of the batch lands inside the batch's buffer, before its padding. */
    st::basic_vector<std::size_t, allocator_<std::size_t>> aligned_offsets_in_buffer;

    /** How many of the batch's locations name a value. */
    std::size_t stored_values_found = 0;

    /** Bytes the batch's buffer needs for every value it found. */
    std::size_t aligned_buffer_size_bytes = 0;

    stored_value_locations() noexcept = default;

    /** A batch of @p count locations, charged to @p allocator, or the status that refused them. */
    static expected<stored_value_locations> make(std::size_t count,
                                                 allocator_<stored_value_location_t> allocator = {}) noexcept {
        stored_value_locations batch;
        batch.locations = st::basic_vector<stored_value_location_t, allocator_<stored_value_location_t>> {allocator};
        batch.aligned_offsets_in_buffer = st::basic_vector<std::size_t, allocator_<std::size_t>> {
            allocator_<std::size_t> {allocator}};
        if (st::status_t const status = batch.locations.resize(count); status != st::status_t::success_k)
            return to_status(status);
        if (st::status_t const status = batch.aligned_offsets_in_buffer.resize(count);
            status != st::status_t::success_k)
            return to_status(status);
        return batch;
    }

    /** Locations in the batch, whether or not they name a value. */
    std::size_t count() const noexcept { return locations.size(); }

    /** Locations of the batch that name a value. */
    std::size_t found_count() const noexcept { return stored_values_found; }

    /** Bytes the batch's buffer needs for every value it found. */
    std::size_t total_size_bytes() const noexcept { return aligned_buffer_size_bytes; }

    /** Whether the location at @p index names a value. */
    bool exists(std::size_t index) const noexcept { return bool(locations[index]); }

    /** Bytes the value at @p index occupies, which must exist. */
    stored_value_size_t size(std::size_t index) const noexcept {
        assert(locations[index] && "Stored value doesn't exist");
        return locations[index].size();
    }

    /** Where the value at @p index lands inside the batch's buffer, which must exist. */
    stored_value_size_t offset_in_buffer(std::size_t index) const noexcept {
        assert(locations[index] && "Stored value doesn't exist");
        return aligned_offsets_in_buffer[index] + locations[index].buffer_padding();
    }

    /** The batch as a span whoever reads or writes it borrows. */
    stored_value_locations_span_t span() noexcept {
        return {std::span {locations}, std::span {aligned_offsets_in_buffer}, stored_values_found,
                aligned_buffer_size_bytes};
    }

    /** The batch as a view whoever reads it borrows. */
    stored_value_locations_view_t view() const noexcept {
        return {std::span {locations}, std::span {aligned_offsets_in_buffer}, stored_values_found,
                aligned_buffer_size_bytes};
    }

    /** Lays the batch out inside one buffer, and counts the values it found. */
    void recalculate_metadata() noexcept { span().recalculate_metadata(); }
};

/**
 *  @brief A batch of locations of whichever layout its collection carries, borrowed.
 *
 *  Every member forwards to the alternative the batch holds, so a caller that does not know the
 *  layout still reads sizes and offsets without a virtual call.
 */
template <locations_mutability_t mutability_ = locations_mutability_t::mutable_k>
class any_stored_value_locations_span {
  public:
    using variant_t = std::variant<stored_value_locations_span<value_layout_t::unknown_k, mutability_>,
                                   stored_value_locations_span<value_layout_t::inline_1_byte_k, mutability_>,
                                   stored_value_locations_span<value_layout_t::inline_4_bytes_k, mutability_>,
                                   stored_value_locations_span<value_layout_t::inline_8_bytes_k, mutability_>,
                                   stored_value_locations_span<value_layout_t::inline_16_bytes_k, mutability_>,
                                   stored_value_locations_span<value_layout_t::inline_32_bytes_k, mutability_>,
                                   stored_value_locations_span<value_layout_t::inline_64_bytes_k, mutability_>,
                                   stored_value_locations_span<value_layout_t::fixed_width_k, mutability_>,
                                   stored_value_locations_span<value_layout_t::variable_width_k, mutability_>>;

    /** The batch of whichever layout the collection carries. */
    variant_t variant;

    /** Locations in the batch, whether or not they name a value. */
    std::size_t count() const noexcept {
        return std::visit([](auto const& locations) { return locations.count(); }, variant);
    }

    /** Locations of the batch that name a value. */
    std::size_t found_count() const noexcept {
        return std::visit([](auto const& locations) { return locations.found_count(); }, variant);
    }

    /** Bytes the batch's buffer needs for every value it found. */
    std::size_t total_size_bytes() const noexcept {
        return std::visit([](auto const& locations) { return locations.total_size_bytes(); }, variant);
    }

    /** Whether the location at @p index names a value. */
    bool exists(std::size_t index) const noexcept {
        return std::visit([index](auto const& locations) { return locations.exists(index); }, variant);
    }

    /** Bytes the value at @p index occupies, which must exist. */
    stored_value_size_t size(std::size_t index) const noexcept {
        return std::visit([index](auto const& locations) { return locations.size(index); }, variant);
    }

    /** Where the value at @p index lands inside the batch's buffer, which must exist. */
    stored_value_size_t offset_in_buffer(std::size_t index) const noexcept {
        return std::visit([index](auto const& locations) { return locations.offset_in_buffer(index); }, variant);
    }

    /** Lays the batch out inside one buffer, and counts the values it found. */
    void recalculate_metadata() noexcept {
        std::visit([](auto& locations) { locations.recalculate_metadata(); }, variant);
    }
};

/** A borrowed batch of locations of whichever layout its collection carries. */
using any_stored_value_locations_span_t = any_stored_value_locations_span<>;

/** A read-only batch of locations of whichever layout its collection carries. */
using any_stored_value_locations_view_t = any_stored_value_locations_span<locations_mutability_t::immutable_k>;

/** A batch of locations of whichever layout its collection carries, with the arrays it owns. */
template <template <typename> typename allocator_ = heap_allocator>
class any_stored_value_locations {
  public:
    using variant_t = std::variant<stored_value_locations<value_layout_t::unknown_k, allocator_>,
                                   stored_value_locations<value_layout_t::inline_1_byte_k, allocator_>,
                                   stored_value_locations<value_layout_t::inline_4_bytes_k, allocator_>,
                                   stored_value_locations<value_layout_t::inline_8_bytes_k, allocator_>,
                                   stored_value_locations<value_layout_t::inline_16_bytes_k, allocator_>,
                                   stored_value_locations<value_layout_t::inline_32_bytes_k, allocator_>,
                                   stored_value_locations<value_layout_t::inline_64_bytes_k, allocator_>,
                                   stored_value_locations<value_layout_t::fixed_width_k, allocator_>,
                                   stored_value_locations<value_layout_t::variable_width_k, allocator_>>;

    /** The batch of whichever layout the collection carries. */
    variant_t variant;

    /** Locations in the batch, whether or not they name a value. */
    std::size_t count() const noexcept {
        return std::visit([](auto const& locations) { return locations.count(); }, variant);
    }

    /** Locations of the batch that name a value. */
    std::size_t found_count() const noexcept {
        return std::visit([](auto const& locations) { return locations.found_count(); }, variant);
    }

    /** Bytes the batch's buffer needs for every value it found. */
    std::size_t total_size_bytes() const noexcept {
        return std::visit([](auto const& locations) { return locations.total_size_bytes(); }, variant);
    }

    /** Whether the location at @p index names a value. */
    bool exists(std::size_t index) const noexcept {
        return std::visit([index](auto const& locations) { return locations.exists(index); }, variant);
    }

    /** Bytes the value at @p index occupies, which must exist. */
    stored_value_size_t size(std::size_t index) const noexcept {
        return std::visit([index](auto const& locations) { return locations.size(index); }, variant);
    }

    /** Where the value at @p index lands inside the batch's buffer, which must exist. */
    stored_value_size_t offset_in_buffer(std::size_t index) const noexcept {
        return std::visit([index](auto const& locations) { return locations.offset_in_buffer(index); }, variant);
    }

    /** The batch as a span whoever reads or writes it borrows. */
    any_stored_value_locations_span_t span() noexcept {
        return std::visit([](auto& locations) { return any_stored_value_locations_span_t {locations.span()}; },
                          variant);
    }

    /** The batch as a view whoever reads it borrows. */
    any_stored_value_locations_view_t view() const noexcept {
        return std::visit([](auto const& locations) { return any_stored_value_locations_view_t {locations.view()}; },
                          variant);
    }

    /** Lays the batch out inside one buffer, and counts the values it found. */
    void recalculate_metadata() noexcept {
        std::visit([](auto& locations) { locations.recalculate_metadata(); }, variant);
    }
};

/** Where one key's value sits, as a sorted run's metadata records it. */
template <typename stored_key_type_>
struct stored_value_md {
    using stored_key_t = stored_key_type_;

    /** The key the value belongs to. */
    stored_key_t stored_key;

    /** The disk holding the file. */
    disk_id_t disk_id;

    /** The file holding the value. */
    file_id_t file_id;

    /** Where the value starts inside that file. */
    stored_value_offset_t offset;

    /** Bytes the value occupies. */
    stored_value_size_t size;
};

} // namespace unum::ustore::nvme
