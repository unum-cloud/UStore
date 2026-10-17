/**
 *  @file include/ustore/blobs/nvme_key_slot.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The vocabulary of UStoreNVMe's LSM trees: identifiers and value layouts, the keys key slots are ordered
 *      by, stored values and their locations, key slots, and the sorted run index laid over them.
 *
 *  A key slot is a stored key with the size and location of its stored value, specialized per value layout. Keys
 *  carry their bounds through @c limit, and the 16-byte key is ordered like @c memcmp. The sorted run index is
 *  the B-tree whose cells hold a sorted run's key slots, which both compactors lay out the same way.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint16_t`, `std::uint64_t`

#include <concepts>    // `std::convertible_to`, `std::same_as`
#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::conditional_t`, `std::is_arithmetic_v`, `std::is_trivially_copyable_v`
#include <utility>     // `std::move`
#include <variant>     // `std::variant`, `std::visit`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`
#include <smashtable/shared.hpp>       // `st::hash`

#include "ustore/utilities/nvme_ring.hpp" // `disk_id_t`, `file_id_t`, `uuid_t`, `string_t`, `bytes_span_t`
#include "ustore/utilities/types.hpp"     // `status_t`, `status_code_t`, `expected`, `byte_t`, the rounding helpers

namespace unum::ustore::blobs {

using utilities::buffer_alignment_k;
using utilities::byte_t;
using utilities::bytes_span_t;
using utilities::bytes_view_t;
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

/** Branching factor of the on-disk B-trees. */
inline constexpr std::size_t btree_branching_k = 16;

#pragma region Identifiers

/** The ID of one collection: the UUID the C ABI addresses it by, unique across every database. */
using collection_id_t = ustore_collection_id_t;

/** The ID of one single-collection database, which its schema records. */
using single_collection_database_id_t = uuid_t;

/** The ID of one database, which its schema records. */
using database_id_t = uuid_t;

/** The identifier of one snapshot inside its database. */
using snapshot_id_t = std::uint64_t;

/** The identifier of one transaction inside its database. */
using transaction_id_t = std::uint64_t;

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

/** The identifier of no transaction. */
inline constexpr transaction_id_t invalid_transaction_id_k = std::numeric_limits<transaction_id_t>::max();

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

/** Whether files reach their destination as copies or leave their source. */
enum class transfer_mode_t : bool {

    /** Copies the files, keeping the source. */
    copy_k,

    /** Moves the files, removing the source. */
    move_k,
};

/** How values of a collection are stored: inline values of a width, fixed-size or variable-size. */
enum class value_layout_t : std::uint16_t {
    unknown_k = 0,

    inline_1_byte_k = 1,
    inline_4_bytes_k = 4,
    inline_8_bytes_k = 8,
    inline_16_bytes_k = 16,
    inline_32_bytes_k = 32,
    inline_64_bytes_k = 64,

    fixed_width_k,
    variable_width_k,
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

/** Whether @p value_layout stores values of that width inside the key slot itself. */
constexpr bool is_inline(value_layout_t value_layout) noexcept {
    return value_layout == value_layout_t::inline_1_byte_k || value_layout == value_layout_t::inline_4_bytes_k ||
           value_layout == value_layout_t::inline_8_bytes_k || value_layout == value_layout_t::inline_16_bytes_k ||
           value_layout == value_layout_t::inline_32_bytes_k || value_layout == value_layout_t::inline_64_bytes_k;
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
    if (value_size == 1) return value_layout_t::inline_1_byte_k;
    if (value_size <= 4) return value_layout_t::inline_4_bytes_k;
    if (value_size <= 8) return value_layout_t::inline_8_bytes_k;
    if (value_size <= 16) return value_layout_t::inline_16_bytes_k;
    if (value_size <= 32) return value_layout_t::inline_32_bytes_k;
    if (value_size <= 64) return value_layout_t::inline_64_bytes_k;
    return value_layout_t::fixed_width_k;
}

#pragma endregion Value Layouts

#pragma endregion Identifiers and Layouts

#pragma region Stored Key

/**
 *  @brief A 16-byte key held as two words, each read big-endian from its eight bytes.
 *
 *  Comparing @c high and then @c low as unsigned words orders keys exactly as @c memcmp orders their bytes,
 *  so an integer identity below 2^64 keeps @c high zero and orders as its value.
 */
struct uuid_stored_key_t {

    /** The first eight bytes, read big-endian. */
    std::uint64_t high = 0;

    /** The last eight bytes, read big-endian. */
    std::uint64_t low = 0;

    /** Reads a key from the 16 bytes at @p bytes. */
    static constexpr uuid_stored_key_t from_bytes(std::uint8_t const* bytes) noexcept {
        uuid_stored_key_t key;
        for (int index = 0; index != 8; ++index) {
            key.high = (key.high << 8) | bytes[index];
            key.low = (key.low << 8) | bytes[8 + index];
        }
        return key;
    }

    /** Writes the key's 16 bytes to @p bytes. */
    constexpr void to_bytes(std::uint8_t* bytes) const noexcept {
        for (int index = 0; index != 8; ++index) {
            bytes[index] = static_cast<std::uint8_t>(high >> (56 - 8 * index));
            bytes[8 + index] = static_cast<std::uint8_t>(low >> (56 - 8 * index));
        }
    }

    /** Whether both keys hold the same 16 bytes. */
    friend constexpr bool operator==(uuid_stored_key_t const& first, uuid_stored_key_t const& second) noexcept {
        return (first.high == second.high) & (first.low == second.low);
    }

    /** Whether @p first orders before @p second, as @c memcmp orders their bytes. */
    friend constexpr bool operator<(uuid_stored_key_t const& first, uuid_stored_key_t const& second) noexcept {
        return (first.high < second.high) | ((first.high == second.high) & (first.low < second.low));
    }
};

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

/** Bounds of a 16-byte key: all bytes zero and all bytes one. */
template <>
struct limit<uuid_stored_key_t> {

    /** The key of 16 zero bytes. */
    static constexpr uuid_stored_key_t min() noexcept { return {0, 0}; }

    /** The key of 16 bytes with every bit set. */
    static constexpr uuid_stored_key_t max() noexcept { return {~std::uint64_t {0}, ~std::uint64_t {0}}; }
};

/** Bounds of the C ABI's key: the nil key and the key of every bit set. */
template <>
struct limit<ustore_key_t> {

    /** The nil key. */
    static constexpr ustore_key_t min() noexcept { return utilities::smallest_key_k; }

    /** The key of 16 bytes with every bit set. */
    static constexpr ustore_key_t max() noexcept { return utilities::largest_key_k; }
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

/** Hashes a key a key slot carries: a 16-byte key by folding its two words, any other through SmashTable's own. */
struct stored_key_hash_t {
    template <typename stored_key_type_>
    std::size_t operator()(stored_key_type_ const& key) const noexcept {
        return st::hash<stored_key_type_> {}(key);
    }
    std::size_t operator()(uuid_stored_key_t const& key) const noexcept {
        return st::hash<std::uint64_t> {}(key.high ^ (key.low * 0x9E3779B97F4A7C15ull));
    }
    std::size_t operator()(ustore_key_t const& key) const noexcept { return utilities::uuid_hash_t {}(key); }
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

    /** Whether the location names a value, which a tombstone does not. */
    explicit operator bool() const noexcept { return offset != tombstone_offset_k; }

    /** Bytes the value occupies. */
    stored_value_size_t size() const noexcept { return size_bytes; }

    /** Bytes a buffer needs for the value, rounded out to what unbuffered I/O accepts. */
    stored_value_size_t aligned_buffer_size() const noexcept {
        bool const is_nonempty = bool(*this) && offset != empty_stored_value_offset_k;
        stored_value_size_t const padded_size = stored_value_size_t(is_nonempty) * (buffer_padding() + size_bytes);
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

#pragma endregion Stored Value

#pragma region Storage Limits

/** Size and capacity limits enforced when validating configurations. */
struct storage_limits_t {
    static constexpr std::size_t fixed_value_min_size_k = 1;
    static constexpr std::size_t fixed_value_max_size_k = 4 * 1024 * 1024;
    static constexpr std::size_t variable_value_min_size_k = 0;
    static constexpr std::size_t variable_value_max_size_k = 4 * 1024 * 1024;
    static constexpr std::size_t cache_limit_max_size_k = 100 * 1024 * 1024 * 1024ull;
    static constexpr std::size_t write_buffer_min_capacity_k = 1024;
    static constexpr std::size_t write_buffer_max_capacity_k = 1024 * 1024 * 1024ull;
    static constexpr std::size_t write_buffer_elements_min_count_k = 16;
    static constexpr std::size_t write_buffer_elements_max_count_k = 1024 * 1024;
    static constexpr std::size_t transaction_write_buffer_min_capacity_k = 1;
    static constexpr std::size_t transaction_write_buffer_max_capacity_k = write_buffer_max_capacity_k;
    static constexpr std::size_t transaction_write_buffer_elements_min_count_k = 1;
    static constexpr std::size_t transaction_write_buffer_elements_max_count_k = write_buffer_elements_max_count_k;
    static constexpr std::size_t first_level_min_capacity_bytes_k = 1024 * 1024;
    static constexpr std::size_t first_level_max_sorted_runs_count_k = 1024;

    /** Runs of level zero that make it due for compaction, as each one is another merge input for every read and
     *  every scan of the collection, whatever bytes they hold. */
    static constexpr std::size_t first_level_compaction_threshold_k = 4;
    static constexpr std::size_t level_min_enlarge_factor_k = 2;
    static constexpr std::size_t level_max_enlarge_factor_k = 64;
    static constexpr std::size_t sorted_run_file_max_size_k = 4 * 1024 * 1024 * 1024ull - 4096;
    static constexpr std::size_t gpu_compaction_threshold_k = 900'000;
    static constexpr std::size_t value_max_size_k = std::max(fixed_value_max_size_k, variable_value_max_size_k);
    static constexpr std::size_t user_data_max_size_k = 4 * 1024 * 1024;
};

static_assert(storage_limits_t::sorted_run_file_max_size_k <= empty_stored_value_offset_k,
              "A stored value's location never reaches the offsets that mean empty and erased");

#pragma endregion Storage Limits

#pragma region Key Slot

template <typename stored_key_, value_layout_t value_layout_>
struct key_slot {
    using stored_key_t = stored_key_;
    static constexpr value_layout_t value_layout_k = value_layout_;

    stored_key_t stored_key = limit<stored_key_t>::max();
    stored_value_size_t size = 0;
    byte_t data[static_cast<std::uint16_t>(value_layout_k)] = {byte_t(0)};

    constexpr bool is_tombstone() const noexcept { return size == 0; }
    constexpr void make_tombstone() noexcept { size = 0; }
    constexpr bool is_stored_value_empty() const noexcept { return size == 0; }

    stored_value_size_t aligned_buffer_size() const noexcept { return size; }
    stored_value_offset_t aligned_offset_in_file() const noexcept { return 0; }
    stored_value_size_t buffer_padding() const noexcept { return 0; }

    constexpr bool operator==(key_slot const& other) const noexcept { return stored_key == other.stored_key; }
    constexpr bool operator==(stored_key_t other) const noexcept { return stored_key == other; }
    constexpr bool operator<(key_slot const& other) const noexcept { return stored_key < other.stored_key; }
    constexpr bool operator<(stored_key_t other) const noexcept { return stored_key < other; }
};

template <typename stored_key_>
struct key_slot<stored_key_, value_layout_t::fixed_width_k> {
    using stored_key_t = stored_key_;
    static constexpr value_layout_t value_layout_k = value_layout_t::fixed_width_k;

    stored_key_t stored_key = limit<stored_key_t>::max();
    stored_value_size_t size = 0;
    stored_value_offset_t location = 0;

    constexpr bool is_tombstone() const noexcept { return size == 0; }
    constexpr void make_tombstone() noexcept { size = 0; }
    constexpr bool is_stored_value_empty() const noexcept { return size == 0; }

    stored_value_size_t aligned_buffer_size() const noexcept {
        auto padded_size = stored_value_offset_t(size != 0) * (buffer_padding() + size);
        return round_up_to_multiple<stored_value_size_t>(padded_size, buffer_alignment_k);
    }
    stored_value_offset_t aligned_offset_in_file() const noexcept {
        return round_down_to_multiple<stored_value_offset_t>(location, buffer_alignment_k);
    }
    stored_value_size_t buffer_padding() const noexcept { return location - aligned_offset_in_file(); }

    constexpr bool operator==(key_slot const& other) const noexcept { return stored_key == other.stored_key; }
    constexpr bool operator==(stored_key_t other) const noexcept { return stored_key == other; }
    constexpr bool operator<(key_slot const& other) const noexcept { return stored_key < other.stored_key; }
    constexpr bool operator<(stored_key_t other) const noexcept { return stored_key < other; }
};

template <typename stored_key_>
struct key_slot<stored_key_, value_layout_t::variable_width_k> {
    using stored_key_t = stored_key_;
    static constexpr value_layout_t value_layout_k = value_layout_t::variable_width_k;

    stored_key_t stored_key = limit<stored_key_t>::max();
    stored_value_size_t size = 0;
    stored_value_offset_t location = 0;

    constexpr bool is_tombstone() const noexcept { return location == tombstone_offset_k; }
    constexpr void make_tombstone() noexcept {
        size = 0;
        location = tombstone_offset_k;
    }
    constexpr bool is_stored_value_empty() const noexcept {
        return (location == tombstone_offset_k) | (location == empty_stored_value_offset_k);
    }

    stored_value_size_t aligned_buffer_size() const noexcept {
        auto padded_size = stored_value_offset_t(!is_stored_value_empty()) * (buffer_padding() + size);
        return round_up_to_multiple<stored_value_size_t>(padded_size, buffer_alignment_k);
    }
    stored_value_offset_t aligned_offset_in_file() const noexcept {
        return round_down_to_multiple<stored_value_offset_t>(location, buffer_alignment_k);
    }
    stored_value_size_t buffer_padding() const noexcept { return location - aligned_offset_in_file(); }

    constexpr bool operator==(key_slot const& other) const noexcept { return stored_key == other.stored_key; }
    constexpr bool operator==(stored_key_t other) const noexcept { return stored_key == other; }
    constexpr bool operator<(key_slot const& other) const noexcept { return stored_key < other.stored_key; }
    constexpr bool operator<(stored_key_t other) const noexcept { return stored_key < other; }
};

#pragma endregion Key Slot

#pragma region Sorted Run Index

/** Levels a sorted run index layout describes, far beyond any sorted run index a sorted run file holds. */
inline constexpr std::uint64_t sorted_run_index_levels_capacity_k = 32;

/**
 *  @brief Maps between a sorted run index's cells and the sorted order of the key slots they hold.
 *
 *  Cells form rows of @c branching stored breadth-first, row @c p parenting rows @c p*(branching+1)+1 through
 *  @c p*(branching+1)+branching+1, and an in-order walk visits the cells in ascending order. Only sorted positions
 *  below @c key_slots_count hold key slots; the positions past them up to @c cells_count are padding.
 */
struct sorted_run_index_layout_t {

    /** Key slots in the sorted run index. */
    std::uint64_t key_slots_count = 0;

    /** Cells in one row. */
    std::uint64_t branching = 0;

    /** Rows of cells, enough for every key slot. */
    std::uint64_t rows_count = 0;

    /** Levels of rows, the root counting as one, and zero for a layout of no key slots. */
    std::uint64_t height = 0;

    /** Cells in the deepest level. */
    std::uint64_t leaf_cells_count = 0;

    /** Rows in the deepest level. */
    std::uint64_t leaf_rows_count = 0;

    /** At zero the key slots count, and at @c index above zero the cells of the top @c height minus @c index levels. */
    std::uint64_t cells_above[sorted_run_index_levels_capacity_k] = {};

    /** At @c index above zero the cells of level @c index minus one in a complete sorted run index, the root being
     *  level zero. */
    std::uint64_t level_cells[sorted_run_index_levels_capacity_k] = {};

    /** Sorted positions between neighbouring cells of a level in a complete sorted run index. */
    std::uint64_t level_stride[sorted_run_index_levels_capacity_k] = {};

    constexpr sorted_run_index_layout_t() noexcept = default;

    /** Lays out @p key_slots key slots in rows of @p cells_per_row, which must be two or more. */
    constexpr sorted_run_index_layout_t(std::uint64_t key_slots, std::uint64_t cells_per_row) noexcept
        : key_slots_count(key_slots), branching(cells_per_row), rows_count(divide_round_up(key_slots, cells_per_row)) {
        if (key_slots == 0) return;
        std::uint64_t levels = 1;
        for (std::uint64_t reach = branching + 1; reach <= key_slots && levels + 1 < sorted_run_index_levels_capacity_k;
             reach *= branching + 1)
            ++levels;
        height = levels;

        std::uint64_t const upper_cells = power_(branching + 1, height - 1) - 1;
        leaf_cells_count = rows_count * branching - upper_cells;
        leaf_rows_count = rows_count - upper_cells / branching;

        cells_above[0] = key_slots;
        for (std::uint64_t index = 1; index <= height; ++index)
            cells_above[index] = power_(branching + 1, height - index) - 1;

        level_cells[1] = cells_above[height - 1];
        level_stride[height - 1] = 1;
        for (std::uint64_t index = 2; index <= height; ++index) {
            level_cells[index] = cells_above[height - index] - cells_above[height - index + 1];
            level_stride[index - 2] = cells_above[index - 1] + 1;
        }
    }

    /** Cells in the sorted run index, padding included. */
    constexpr std::uint64_t cells_count() const noexcept { return rows_count * branching; }

    /** Level of the row holding @p cell, the root being level zero. */
    constexpr std::uint64_t level_of(std::uint64_t cell) const noexcept {
        for (std::uint64_t index = 1; index < height; ++index)
            if (cells_above[index] <= cell) return height - index;
        return 0;
    }

    /** Whether the layout describes any key slot, which decides whether its two mappings mean anything. */
    constexpr bool holds_key_slots() const noexcept { return height != 0; }

    /** Sorted position of @p cell, at or past @c key_slots_count for padding and for an empty layout. */
    constexpr std::uint64_t cell_to_sorted(std::uint64_t cell) const noexcept {
        if (!holds_key_slots()) return key_slots_count;
        std::uint64_t const level = level_of(cell);
        std::uint64_t const offset_in_level = cell - cells_above[height - level];
        std::uint64_t const slots = offset_in_level + offset_in_level / branching + 1;
        std::uint64_t const leaf_cells_before = level_cells[height - level - 1] * slots;
        std::uint64_t const missing_leaf_cells = leaf_cells_before < leaf_cells_count
                                                     ? 0
                                                     : leaf_cells_before - leaf_cells_count;
        return slots * level_stride[level] - 1 - missing_leaf_cells;
    }

    /** Cell holding sorted position @p sorted, which is @c cells_count for an empty layout that holds none. */
    constexpr std::uint64_t sorted_to_cell(std::uint64_t sorted) const noexcept {
        if (!holds_key_slots()) return cells_count();
        std::uint64_t const leaf_row = sorted / (branching + 1);
        std::uint64_t leaf_cells_before = leaf_rows_count * branching;
        if (leaf_row < leaf_rows_count) {
            std::uint64_t const offset_in_row = sorted - leaf_row * (branching + 1);
            if (offset_in_row != branching) return cells_above[1] + leaf_row * branching + offset_in_row;
            leaf_cells_before = (leaf_row + 1) * branching;
        }

        // Without the leaves the sorted run index is complete, so the trailing zeros of the position in base
        // branching+1 name its level
        std::uint64_t level_from_bottom = 0;
        std::uint64_t rows_before = sorted - leaf_cells_before + 1;
        std::uint64_t offset_in_row = 0;
        while (true) {
            std::uint64_t const previous = rows_before;
            rows_before /= branching + 1;
            offset_in_row = previous - rows_before * (branching + 1);
            if (offset_in_row != 0) break;
            ++level_from_bottom;
        }
        return cells_above[level_from_bottom + 2] + rows_before * branching + offset_in_row - 1;
    }

  private:
    static constexpr std::uint64_t power_(std::uint64_t base, std::uint64_t exponent) noexcept {
        std::uint64_t result = 1;
        for (std::uint64_t step = 0; step != exponent; ++step) result *= base;
        return result;
    }
};

#pragma endregion Sorted Run Index

} // namespace unum::ustore::blobs
