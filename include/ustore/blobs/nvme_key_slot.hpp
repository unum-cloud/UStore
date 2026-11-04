/**
 *  @file include/ustore/blobs/nvme_key_slot.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The vocabulary of UStoreNVMe's LSM trees: identifiers and value layouts, the keys key slots are ordered
 *      by, stored values and their locations, key slots, and the sorted run index laid over them.
 *
 *  A key slot is a stored key, with an inline value beside it for the layouts holding values
 *  inline, and the kind of its delta sits in its run's kinds column beside the stamps. A value held
 *  apart is found by the key slot's sorted position: by stride for fixed-width values, and through
 *  the run's value offsets column for variable-width ones. Keys carry their bounds through
 *  @c limit, and the 16-byte key is ordered like @c memcmp. A sorted run keeps its key slots in
 *  sorted order, which both compactors lay out the same way.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint16_t`, `std::uint64_t`

#include <concepts>    // `std::convertible_to`, `std::same_as`
#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::conditional_t`, `std::is_arithmetic_v`, `std::is_trivially_copyable_v`
#include <variant>     // `std::variant`, `std::visit`

#include <smashtable/basic_vector.hpp> // `st::vector`
#include <smashtable/row_layout.hpp>   // `st::key128_t`
#include <smashtable/shared.hpp>       // `st::hash`

#include "ustore/utilities/folds.hpp"       // `delta_kind_t`
#include "ustore/utilities/nvme_ring.hpp"   // `disk_id_t`, `file_id_t`
#include "ustore/utilities/nvme_slots.hpp"  // `slot_bytes_k`, `sorted_run_bytes_max_k`
#include "ustore/utilities/ram_buffers.hpp" // `heap_allocator`, `buffer_alignment_k`
#include "ustore/utilities/types.hpp" // `status_t`, `expected`, `uuid_t`, `bytes_span_t`, `bytes_view_t`, `to_status`

namespace unum::ustore::blobs {

using utilities::buffer_alignment_k;
using utilities::byte_t;
using utilities::bytes_span_t;
using utilities::bytes_view_t;
using utilities::delta_kind_t;
using utilities::disk_id_t;
using utilities::divide_round_up;
using utilities::expected;
using utilities::file_id_t;
using utilities::heap_allocator;
using utilities::round_down_to_multiple;
using utilities::round_up_to_multiple;
using utilities::status_code_t;
using utilities::status_t;
using utilities::to_status;
using utilities::u32_t;
using utilities::u64_t;
using utilities::uuid_t;

#pragma region Identifiers and Layouts

#pragma region Identifiers

/** The ID of one collection: the UUID the C ABI addresses it by, unique across every database. */
using collection_id_t = ustore_collection_id_t;

/** The ID of one single-collection database, which its schema records. */
using single_collection_database_id_t = uuid_t;

/** The ID of one database, which its schema records. */
using database_id_t = uuid_t;

/** The identifier of one snapshot inside its database. */
using snapshot_id_t = std::uint64_t;

/**
 *  @brief The position of one commit in its ledger's order, which a reader reads at and every version carries.
 *
 *  A counter rather than a clock: @c std::chrono::system_clock is neither steady nor distinct across threads, so
 *  two commits could share an instant and one would hide the other's writes.
 */
using transaction_stamp_t = std::uint64_t;

/** The stamp of a key no commit has ever written, which orders before every issued stamp. */
inline constexpr transaction_stamp_t unwritten_stamp_k = 0;

/** The stamp of a transaction that has not started, or of a watch that was never taken. */
inline constexpr transaction_stamp_t invalid_transaction_stamp_k = std::numeric_limits<transaction_stamp_t>::max();

/** The stamp a read at the head answers at, which orders after every stamp a commit draws. */
inline constexpr transaction_stamp_t head_stamp_k = std::numeric_limits<transaction_stamp_t>::max();

/** The identifier of one backup inside its database. */
using backup_id_t = std::uint64_t;

/** Hashes a collection ID as the C ABI does, for the tables a transaction groups its writes in by collection. */
struct collection_id_hash_t {
    std::size_t operator()(collection_id_t const& id) const noexcept { return std::hash<collection_id_t> {}(id); }
};

/** The ID of no collection, every bit set, which no minted UUID spells. */
inline constexpr collection_id_t collection_invalid_id_k {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}};

/** The identifier of no snapshot. */
inline constexpr snapshot_id_t invalid_snapshot_id_k = 0;

/** The identifier of no backup. */
inline constexpr backup_id_t invalid_backup_id_k = std::numeric_limits<backup_id_t>::max();

#pragma endregion Identifiers

#pragma region File Names

/** Extension of sorted run data files. */
inline constexpr std::string_view sorted_run_file_extension_k = "udb";

/** Extension of schema documents. */
inline constexpr std::string_view schema_file_extension_k = "sch";

/** Extension of backup schema documents. */
inline constexpr std::string_view backup_schema_file_extension_k = "bak";

/** Suffix of the file holding user data next to a schema. */
inline constexpr std::string_view user_data_file_name_k = "user.data";

#pragma endregion File Names

#pragma region Modes

/** Whether a database holds one collection with transactions of its own, or many sharing them. */
enum class database_kind_t : std::uint8_t {
    unknown_k = 0x00,
    single_collection_database_k = 0x01,
    database_k = 0x02,
};

/** What a batch lookup does with the location of a key it does not find. */
enum class missing_locations_t : bool {

    /** Resets the location, so it reports the key as missing. */
    invalidate_k,

    /** Leaves the location as the caller filled it. */
    keep_k,
};

/** What a lookup of one key found, named after the kind of the newest delta carrying it. */
enum class lookup_result_t : std::uint8_t {

    /** No delta carries the key. */
    missing_k,

    /** A write delta carries the whole value, or a fold completed it. */
    written_k,

    /** A remove delta carries the key, so it holds no value. */
    removed_k,

    /** A change delta carries the key, which older deltas complete. */
    changed_k,

    /** The medium moved fewer bytes than the delta claims, so this one key failed. */
    incomplete_k,
};

/** Whether a run keeps the bytes of each key past its 16-byte head beside its key slots, as strings runs do. */
enum class key_tails_t : bool { absent_k = false, carried_k = true };

/** Whether files reach their destination as copies or leave their source. */
enum class transfer_mode_t : bool {

    /** Copies the files, keeping the source. */
    copy_k,

    /** Moves the files, removing the source. */
    move_k,
};

/** How values of a collection are stored: inline values of a width, fixed-size, variable-size, or one 16-byte key. */
enum class value_layout_t : std::uint16_t {
    unknown_k = 0,

    /** Values of up to 12 bytes inside a 32-byte key slot, after its 16-byte key and 4-byte size. */
    inline_12_bytes_k = 12,

    /** Values of up to 44 bytes inside a 64-byte key slot, after its 16-byte key and 4-byte size. */
    inline_44_bytes_k = 44,

    /** Values of one width outside a 16-byte key slot; the numbers below are persisted in store tags. */
    fixed_width_k = 65,

    /** Values of differing widths outside a 16-byte key slot. */
    variable_width_k = 66,

    /** Every value is one 16-byte key, held in the key slot: the strings modality's layout. */
    uuid_k = 67,
};

/** Bit fields describing a sequence of values: completeness, ordering, sorting and tree shape. */
enum class data_structure_info_t : std::uint16_t {
    unknown_k = 0x0000,

    complete_k = 0x0000,
    incomplete_k = 0xF000,

    ordered_k = 0x0000,
    unordered_k = 0x0F00,

    unsorted_k = 0x0000,
    sorted_k = 0x00F0,

    sequence_k = 0x0000,
    tree_k = 0x000F
};

#pragma endregion Modes

#pragma region Value Layouts

/** Whether @p value_layout stores values inside the key slot itself, of its width or as one 16-byte key. */
constexpr bool is_inline(value_layout_t value_layout) noexcept {
    return value_layout == value_layout_t::inline_12_bytes_k || value_layout == value_layout_t::inline_44_bytes_k ||
           value_layout == value_layout_t::uuid_k;
}

/** Whether @p value_layout stores values of one width outside the key slot. */
constexpr bool is_fixed(value_layout_t value_layout) noexcept { return value_layout == value_layout_t::fixed_width_k; }

/** Whether @p value_layout stores values of differing widths outside the key slot. */
constexpr bool is_variable_width(value_layout_t value_layout) noexcept {
    return value_layout == value_layout_t::variable_width_k;
}

/** Whether @p value_layout stores values outside the key slot, of one width or of many. */
constexpr bool is_fixed_or_variable_width(value_layout_t value_layout) noexcept {
    return is_fixed(value_layout) || is_variable_width(value_layout);
}

/** The narrowest value layout holding values of @p value_size bytes, where zero means variable-size values. */
constexpr value_layout_t resolve_value_layout(std::size_t value_size) noexcept {
    if (value_size == 0) return value_layout_t::variable_width_k;
    if (value_size <= 12) return value_layout_t::inline_12_bytes_k;
    if (value_size <= 44) return value_layout_t::inline_44_bytes_k;
    return value_layout_t::fixed_width_k;
}

#pragma endregion Value Layouts

#pragma endregion Identifiers and Layouts

#pragma region Stored Key

/** The least and the greatest value of a key type, one specialization per type. */
template <typename value_type_>
struct limit;

/** Bounds of an arithmetic key, as @c std::numeric_limits reports them. */
template <typename value_type_>
    requires std::is_arithmetic_v<value_type_>
struct limit<value_type_> {

    /** The least value. */
    static constexpr value_type_ min() noexcept { return min_k; }

    /** The greatest value. */
    static constexpr value_type_ max() noexcept { return max_k; }

  private:
    static constexpr value_type_ min_k = std::numeric_limits<value_type_>::lowest();
    static constexpr value_type_ max_k = std::numeric_limits<value_type_>::max();
};

/** Bounds of a 16-byte key, whose two words order as @c memcmp orders its bytes: all bytes zero and all bytes one. */
template <>
struct limit<st::key128_t> {

    /** The key of 16 zero bytes. */
    static constexpr st::key128_t min() noexcept { return {0, 0}; }

    /** The key of 16 bytes with every bit set. */
    static constexpr st::key128_t max() noexcept { return {~std::uint64_t {0}, ~std::uint64_t {0}}; }
};

/** Bounds of a composite key that names its own, through static @c min and @c max members. */
template <typename value_type_>
    requires requires {
        { value_type_::min() } -> std::same_as<value_type_>;
        { value_type_::max() } -> std::same_as<value_type_>;
    }
struct limit<value_type_> {

    /** The least value. */
    static constexpr value_type_ min() noexcept { return value_type_::min(); }

    /** The greatest value. */
    static constexpr value_type_ max() noexcept { return value_type_::max(); }
};

/** Hashes a key a key slot carries through SmashTable's own hash. */
struct stored_key_hash_t {
    template <typename stored_key_type_>
    std::size_t operator()(stored_key_type_ const& key) const noexcept {
        return st::hash<stored_key_type_> {}(key);
    }
};

/** A key a key slot carries: trivially copyable, ordered by @c < and @c ==, with bounds from @c limit. */
template <typename stored_key_type_>
concept stored_key_concept = std::is_trivially_copyable_v<stored_key_type_> &&
                             requires(stored_key_type_ const& first, stored_key_type_ const& second) {
                                 { first < second } -> std::convertible_to<bool>;
                                 { first == second } -> std::convertible_to<bool>;
                                 { limit<stored_key_type_>::max() } -> std::same_as<stored_key_type_>;
                             };

#pragma endregion Stored Key

#pragma region Stored Value

/** Where one stored value starts inside the bytes holding it, such as a sorted run's values section. */
using stored_value_offset_t = u32_t;

/** How many bytes one stored value occupies. */
using stored_value_size_t = u32_t;

/** The bytes of one stored value a caller may write into. */
using stored_value_span_t = bytes_span_t;

/** The bytes of one stored value a caller may only read. */
using stored_value_view_t = bytes_view_t;

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

    /** The kind of the delta found there, so a removal and an empty value differ by kind. */
    delta_kind_t kind = delta_kind_t::write_k;

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

    /** Whether the location names a value, which a removal does not. */
    explicit operator bool() const noexcept { return kind != delta_kind_t::remove_k; }

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

    /** Whether the location names a value, which a removal does not. */
    explicit operator bool() const noexcept { return kind != delta_kind_t::remove_k; }

    /** Bytes the value occupies. */
    stored_value_size_t size() const noexcept { return size_bytes; }

    /** Bytes a buffer needs for the value, rounded out to what unbuffered I/O accepts. */
    stored_value_size_t aligned_buffer_size() const noexcept {
        stored_value_size_t const padded_size = stored_value_size_t(size_bytes != 0) * (buffer_padding() + size_bytes);
        return round_up_to_multiple<stored_value_size_t>(padded_size, buffer_alignment_k);
    }

    /** Where an unbuffered read of the value starts inside the file. */
    stored_value_size_t aligned_offset_in_file() const noexcept {
        return round_down_to_multiple<stored_value_offset_t>(offset, buffer_alignment_k);
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

    /** Whether the location names a value, which a removal does not. */
    explicit operator bool() const noexcept { return kind != delta_kind_t::remove_k; }

    /** Bytes the value occupies. */
    stored_value_size_t size() const noexcept { return size_bytes; }

    /** Bytes a buffer needs for the value, rounded out to what unbuffered I/O accepts. */
    stored_value_size_t aligned_buffer_size() const noexcept {
        stored_value_size_t const padded_size = stored_value_size_t(size_bytes != 0) * (buffer_padding() + size_bytes);
        return round_up_to_multiple<stored_value_size_t>(padded_size, buffer_alignment_k);
    }

    /** Where an unbuffered read of the value starts inside the file. */
    stored_value_size_t aligned_offset_in_file() const noexcept {
        return round_down_to_multiple<stored_value_offset_t>(offset, buffer_alignment_k);
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
    st::vector<stored_value_location_t, allocator_<stored_value_location_t>> locations;

    /** Where every value of the batch lands inside the batch's buffer, before its padding. */
    st::vector<std::size_t, allocator_<std::size_t>> aligned_offsets_in_buffer;

    /** How many of the batch's locations name a value. */
    std::size_t stored_values_found = 0;

    /** Bytes the batch's buffer needs for every value it found. */
    std::size_t aligned_buffer_size_bytes = 0;

    stored_value_locations() noexcept = default;

    /** A batch of @p count locations, charged to @p allocator, or the status that refused them. */
    static expected<stored_value_locations> make(std::size_t count,
                                                 allocator_<stored_value_location_t> allocator = {}) noexcept {
        stored_value_locations batch;
        batch.locations = st::vector<stored_value_location_t, allocator_<stored_value_location_t>> {allocator};
        batch.aligned_offsets_in_buffer = st::vector<std::size_t, allocator_<std::size_t>> {
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
                                   stored_value_locations_span<value_layout_t::inline_12_bytes_k, mutability_>,
                                   stored_value_locations_span<value_layout_t::inline_44_bytes_k, mutability_>,
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
                                   stored_value_locations<value_layout_t::inline_12_bytes_k, allocator_>,
                                   stored_value_locations<value_layout_t::inline_44_bytes_k, allocator_>,
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

#pragma endregion Stored Value

#pragma region Storage Limits

/** Size and capacity limits enforced when validating configurations. */
struct storage_limits_t {
    static constexpr std::size_t fixed_value_min_size_k = 1;
    static constexpr std::size_t fixed_value_max_size_k = 4 * 1024 * 1024;
    static constexpr std::size_t variable_value_min_size_k = 0;
    static constexpr std::size_t variable_value_max_size_k = 4 * 1024 * 1024;
    static constexpr std::size_t cache_limit_max_size_k = 100 * 1024 * 1024 * 1024ull;
    static constexpr std::size_t memory_runs_min_bytes_k = 1024;

    /** A flush drains up to twice the memory-run bound, which one run must still hold. */
    static constexpr std::size_t memory_runs_max_bytes_k = utilities::slot_bytes_k / 4;
    static constexpr std::size_t memory_runs_min_deltas_k = 16;
    static constexpr std::size_t memory_runs_max_deltas_k = 1024 * 1024;
    static constexpr std::size_t transaction_min_bytes_k = 1;
    static constexpr std::size_t transaction_max_bytes_k = memory_runs_max_bytes_k;
    static constexpr std::size_t transaction_min_deltas_k = 1;
    static constexpr std::size_t transaction_max_deltas_k = memory_runs_max_deltas_k;
    static constexpr std::size_t level_min_enlarge_factor_k = 2;
    static constexpr std::size_t level_max_enlarge_factor_k = 64;
    static constexpr std::size_t sorted_run_file_max_size_k = utilities::sorted_run_bytes_max_k;
    static constexpr std::size_t cuda_compaction_threshold_k = 900'000;
    static constexpr std::size_t value_max_size_k = std::max(fixed_value_max_size_k, variable_value_max_size_k);
    static constexpr std::size_t user_data_max_size_k = 4 * 1024 * 1024;
};

#pragma endregion Storage Limits

#pragma region Key Slot

template <typename stored_key_type_, value_layout_t value_layout_, key_tails_t key_tails_ = key_tails_t::absent_k>
struct key_slot {
    using stored_key_t = stored_key_type_;
    static constexpr value_layout_t value_layout_k = value_layout_;

    stored_key_t stored_key = limit<stored_key_t>::max();
    stored_value_size_t size = 0;
    byte_t data[static_cast<std::uint16_t>(value_layout_k)] = {byte_t(0)};

    stored_value_size_t aligned_buffer_size() const noexcept { return size; }
    stored_value_offset_t aligned_offset_in_file() const noexcept { return 0; }
    stored_value_size_t buffer_padding() const noexcept { return 0; }

    constexpr bool operator==(key_slot const& other) const noexcept { return stored_key == other.stored_key; }
    constexpr bool operator==(stored_key_t other) const noexcept { return stored_key == other; }
    constexpr bool operator<(key_slot const& other) const noexcept { return stored_key < other.stored_key; }
    constexpr bool operator<(stored_key_t other) const noexcept { return stored_key < other; }
};

/** A key slot of a fixed-width value, which its run finds by stride from the key slot's sorted position. */
template <typename stored_key_type_>
struct key_slot<stored_key_type_, value_layout_t::fixed_width_k> {
    using stored_key_t = stored_key_type_;
    static constexpr value_layout_t value_layout_k = value_layout_t::fixed_width_k;

    stored_key_t stored_key = limit<stored_key_t>::max();

    constexpr bool operator==(key_slot const& other) const noexcept { return stored_key == other.stored_key; }
    constexpr bool operator==(stored_key_t other) const noexcept { return stored_key == other; }
    constexpr bool operator<(key_slot const& other) const noexcept { return stored_key < other.stored_key; }
    constexpr bool operator<(stored_key_t other) const noexcept { return stored_key < other; }
};

/** A key slot of a variable-width value, which its run finds through the value offsets column by sorted position. */
template <typename stored_key_type_, key_tails_t key_tails_>
struct key_slot<stored_key_type_, value_layout_t::variable_width_k, key_tails_> {
    using stored_key_t = stored_key_type_;
    static constexpr value_layout_t value_layout_k = value_layout_t::variable_width_k;
    static constexpr bool carries_tails_k = key_tails_ == key_tails_t::carried_k;

    stored_key_t stored_key = limit<stored_key_t>::max();

    constexpr bool operator==(key_slot const& other) const noexcept { return stored_key == other.stored_key; }
    constexpr bool operator==(stored_key_t other) const noexcept { return stored_key == other; }
    constexpr bool operator<(key_slot const& other) const noexcept { return stored_key < other.stored_key; }
    constexpr bool operator<(stored_key_t other) const noexcept { return stored_key < other; }
};

/** A key slot holding its 16-byte value inline, with no size, as every value has the same one. */
template <typename stored_key_type_, key_tails_t key_tails_>
struct key_slot<stored_key_type_, value_layout_t::uuid_k, key_tails_> {
    using stored_key_t = stored_key_type_;
    static constexpr value_layout_t value_layout_k = value_layout_t::uuid_k;
    static constexpr bool carries_tails_k = key_tails_ == key_tails_t::carried_k;
    static constexpr stored_value_size_t size = 16;

    stored_key_t stored_key = limit<stored_key_t>::max();
    byte_t data[size] {};

    constexpr bool operator==(key_slot const& other) const noexcept { return stored_key == other.stored_key; }
    constexpr bool operator==(stored_key_t other) const noexcept { return stored_key == other; }
    constexpr bool operator<(key_slot const& other) const noexcept { return stored_key < other.stored_key; }
    constexpr bool operator<(stored_key_t other) const noexcept { return stored_key < other; }
};

static_assert(sizeof(key_slot<st::key128_t, value_layout_t::inline_12_bytes_k>) == 32,
              "A small inline cell is its 16-byte key, its 4-byte size and 12 bytes of value");
static_assert(sizeof(key_slot<st::key128_t, value_layout_t::inline_44_bytes_k>) == 64,
              "A wide inline cell is its 16-byte key, its 4-byte size and 44 bytes of value");
static_assert(sizeof(key_slot<st::key128_t, value_layout_t::uuid_k, key_tails_t::carried_k>) == 32,
              "A strings cell is a 16-byte head and its 16-byte value");
static_assert(sizeof(key_slot<st::key128_t, value_layout_t::fixed_width_k>) == 16,
              "A fixed-width cell is its 16-byte key alone");
static_assert(sizeof(key_slot<st::key128_t, value_layout_t::variable_width_k>) == 16,
              "A variable-width cell is its 16-byte key alone");
static_assert(sizeof(key_slot<st::key128_t, value_layout_t::variable_width_k, key_tails_t::carried_k>) == 16,
              "A tailed variable-width cell is its 16-byte head alone");

#pragma endregion Key Slot

} // namespace unum::ustore::blobs
