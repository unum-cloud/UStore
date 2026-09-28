/**
 *  @file src/nvme/types.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Identifiers, failures, allocators, value layout kinds and serialization shared by UStoreNVMe.
 */

#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint16_t`, `std::uint64_t`, `std::uintptr_t`
#include <cstring> // `std::memcpy`

#include <chrono>      // `std::chrono::system_clock`
#include <concepts>    // `std::same_as`
#include <limits>      // `std::numeric_limits`
#include <span>        // `std::span`
#include <functional>  // `std::hash`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_trivially_copyable_v`
#include <utility>     // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`
#include <smashtable/shared.hpp>       // `st::status_t`, `st::default_allocator`
#include <stringzilla/stringzilla.hpp> // `sz::string_t`

#include <ustore/db.h>       // `ustore_status_t`, `ustore_error_t`
#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`, the `st` and `sz` aliases

#include "cuda.hpp" // `cuda_device_allocator`, when the toolkit is on the include path

#include "nvme/aligned_allocator.hpp"     // `aligned_allocator`
#include "nvme/constrained_allocator.hpp" // `resource_constraint_t`, `constrained_allocator`
#include "nvme/countdown.hpp"             // `countdown_t`, `notifier_t`
#include "nvme/uuid.hpp"                  // `uuid_t`, `uuid_generator`

namespace unum::ustore::nvme {

/** Alignment and size multiple of buffers used for direct I/O. */
inline constexpr std::size_t buffer_alignment_k = 4096;

/** Branching factor of the on-disk B-trees. */
inline constexpr std::size_t btree_branching_k = 16;

/** Attempts of a disk read before it fails. */
inline constexpr std::size_t disk_read_attempts_on_failure_k = 2;

/** Attempts of a disk write before it fails. */
inline constexpr std::size_t disk_write_attempts_on_failure_k = 4;

#pragma region Vocabulary

/** Text the engine owns, grown through the fallible members that never throw. */
using string_t = sz::string_t;

/** A view of text the engine does not own. */
using string_view_t = sz::string_view_t;

/** Bytes a caller may write into. */
using bytes_span_t = std::span<std::byte>;

/** Bytes a caller may only read. */
using bytes_view_t = std::span<std::byte const>;

/** The allocator every unbudgeted container reaches, which answers null instead of throwing. */
template <typename value_type_>
using heap_allocator = st::default_allocator<value_type_>;

/** The allocator of buffers unbuffered reads and writes accept. */
template <typename value_type_>
using direct_io_allocator = aligned_allocator<value_type_, buffer_alignment_k>;

/** Bytes charged to no budget, with no alignment beyond the heap's own. */
using buffer_t = st::basic_vector<std::byte>;

/** Bytes of a buffer unbuffered I/O accepts, charged to no budget. */
using aligned_buffer_t = st::basic_vector<std::byte, direct_io_allocator<std::byte>>;

/** Bytes charged to a named budget. */
using constrained_buffer_t = st::basic_vector<std::byte, constrained_allocator<std::byte>>;

/** Bytes of a buffer unbuffered I/O accepts, charged to a named budget. */
using constrained_aligned_buffer_t =
    st::basic_vector<std::byte, constrained_allocator<std::byte, direct_io_allocator<std::byte>>>;

#if __has_include(<cuda_runtime.h>)

/** Bytes of GPU memory, charged to the budget of the device they sit on. */
using constrained_device_buffer_t =
    st::basic_vector<std::byte, constrained_allocator<std::byte, cuda_device_allocator<std::byte>>>;

#endif

#pragma endregion Vocabulary

#pragma region Time

/** The clock every timestamp of the engine is taken from. */
using clock_t = std::chrono::system_clock;

/** One instant on that clock, as a snapshot, a transaction and a watch record it. */
using time_point_t = clock_t::time_point;

/** The instant this call happens at. */
inline time_point_t timestamp() noexcept { return clock_t::now(); }

/** Hashes an instant by the count of its epoch offset, for a table keyed by one. */
struct time_point_hasher_t {

    std::size_t operator()(time_point_t const& instant) const noexcept {
        return std::hash<i64_t> {}(static_cast<i64_t>(instant.time_since_epoch().count()));
    }
};

#pragma endregion Time

#pragma region Identifiers

/** The identifier of one unit of data a backup or a transfer moves. */
using data_unit_id_t = uuid_t;

/** The identifier of one collection, unique across every database. */
using collection_uuid_t = uuid_t;

/** The identifier of one collection as its schema and its files number it. */
using collection_numeric_id_t = std::uint64_t;

/** The name of one collection. */
using collection_alphanumeric_id_t = string_t;

/** The identifier of one single-collection database. */
using single_collection_database_uuid_t = uuid_t;

/** The identifier of one database. */
using database_uuid_t = uuid_t;

/** The identifier of one snapshot inside its database. */
using snapshot_id_t = std::uint64_t;

/** The identifier of one transaction inside its database. */
using transaction_id_t = std::uint64_t;

/** The identifier of one backup inside its database. */
using backup_id_t = std::uint64_t;

/** The name and the numeric identifier of one collection. */
struct collection_ids_t {

    /** The name users address the collection by. */
    collection_alphanumeric_id_t name;

    /** The number its schema and its files carry. */
    collection_numeric_id_t numeric_id;
};

/** The numeric identifier of no collection. */
inline constexpr collection_numeric_id_t collection_invalid_numeric_id_k =
    std::numeric_limits<collection_numeric_id_t>::max();

/** The identifier of no snapshot. */
inline constexpr snapshot_id_t invalid_snapshot_id_k = 0;

/** The identifier of no transaction. */
inline constexpr transaction_id_t invalid_transaction_id_k = std::numeric_limits<transaction_id_t>::max();

/** The identifier of no backup. */
inline constexpr backup_id_t invalid_backup_id_k = std::numeric_limits<backup_id_t>::max();

/** The canonical text of @p identifier, as a schema document carries it. */
inline expected<string_t> uuid_to_string(uuid_t const& identifier) noexcept {
    char text[uuid_t::printed_length_k] = {};
    identifier.print(text);
    string_t printed;
    if (!printed.try_assign(string_view_t {text, uuid_t::printed_length_k}))
        return status_t {status_code_t::out_of_memory_k, "No room for the text of an identifier"};
    return printed;
}

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

#pragma region Failures

/** The status reporting a SmashTable container's @p failure, which every fallible member answers. */
constexpr status_t to_status(st::status_t failure) noexcept {
    switch (failure) {
    case st::status_t::success_k: return {};
    case st::status_t::out_of_memory_heap_k: return {status_code_t::out_of_memory_k, "Out of memory"};
    case st::status_t::consistency_k: return {status_code_t::internal_k, "A container invariant did not hold"};
    case st::status_t::operation_not_permitted_k:
        return {status_code_t::invalid_argument_k, "The object does not admit the call"};
    case st::status_t::operation_would_block_k: return {status_code_t::internal_k, "A bounded retry gave up"};
    case st::status_t::invalid_argument_k: return {status_code_t::invalid_argument_k, "Invalid argument"};
    case st::status_t::capacity_exhausted_k: return {status_code_t::capacity_exhausted_k, "Capacity exhausted"};
    case st::status_t::input_output_k: return {status_code_t::input_output_k, "Input or output failed"};
    case st::status_t::key_already_exists_k: return {status_code_t::already_exists_k, "Key already exists"};
    case st::status_t::key_not_found_k: return {status_code_t::not_found_k, "Key not found"};
    case st::status_t::write_conflict_k: return {status_code_t::write_conflict_k, "Write conflict"};
    case st::status_t::read_conflict_k: return {status_code_t::read_conflict_k, "Read conflict"};
    case st::status_t::phantom_conflict_k: return {status_code_t::phantom_conflict_k, "Phantom conflict"};
    default: return {status_code_t::internal_k, "A container failed for an unknown reason"};
    }
}

#pragma endregion Failures

#pragma region Measurements

/** Memory and disk space one component takes. */
struct storage_info_t {

    /** Bytes mapped into memory. */
    std::size_t mapped_bytes = 0;

    /** Bytes taken on disk. */
    std::size_t disk_bytes = 0;
};

#pragma endregion Measurements

#pragma region Atomic Words

/** A word one thread claims by compare-exchange: readers count upward from zero, the writer holds @c writer_claim_k. */
using claim_word_t = std::int32_t;

/** What a claim word holds while one writer has it, so no reader can add to it. */
constexpr claim_word_t writer_claim_k = -1;

/** Readers still inside a retired slot, which a publisher waits on before freeing it. */
using readers_count_t = std::int32_t;

/** Whether the owner of a background job or a merge still wants it, moved by compare-exchange on a word. */
enum class cancellation_state_t : std::int32_t {
    wanted_k = 0,
    cancelled_k = 1,
};

/** Whether a thread holds the right to compact a tree, moved by compare-exchange on a word. */
enum class compaction_state_t : std::int32_t {
    idle_k = 0,
    compacting_k = 1,
};

#pragma endregion Atomic Words

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

/** Whether both the address and the size of @p buffer are multiples of @c buffer_alignment_k. */
inline bool is_buffer_aligned(bytes_view_t buffer) noexcept {
    auto const address = reinterpret_cast<std::uintptr_t>(buffer.data());
    return address % buffer_alignment_k == 0 && buffer.size() % buffer_alignment_k == 0;
}

#pragma endregion Value Layouts

#pragma region Byte Views

/** The bytes of @p buffer, which a caller may only read. */
template <typename allocator_type_>
inline bytes_view_t as_bytes_view(st::basic_vector<std::byte, allocator_type_> const& buffer) noexcept {
    return bytes_view_t {buffer};
}

/** The bytes of @p buffer, which a caller may write into. */
template <typename allocator_type_>
inline bytes_span_t as_bytes_span(st::basic_vector<std::byte, allocator_type_>& buffer) noexcept {
    return bytes_span_t {buffer};
}

#pragma endregion Byte Views

#pragma region Serialization

/** A value that measures, saves and loads its own bytes, where a successful load consumes its storage requirements. */
template <typename value_type_>
concept serializable_concept = requires(value_type_ const& constant, value_type_& value, bytes_span_t destination,
                                        bytes_view_t source) {
    { constant.storage_requirements() } noexcept -> std::same_as<std::size_t>;
    { constant.save(destination) } noexcept -> std::same_as<status_t>;
    { value.load(source) } noexcept -> std::same_as<status_t>;
};

/** A value stored as its object representation, having no serialization of its own. */
template <typename value_type_>
concept trivially_serializable_concept = std::is_trivially_copyable_v<value_type_> &&
                                         !serializable_concept<value_type_>;

/** The bytes of a trivially serializable value. */
template <trivially_serializable_concept value_type_>
std::size_t storage_requirements(value_type_ const&) noexcept {
    return sizeof(value_type_);
}

/** Copies the object representation of @p value into @p destination. */
template <trivially_serializable_concept value_type_>
status_t save(value_type_ const& value, bytes_span_t destination) noexcept {
    if (destination.size() < sizeof(value_type_))
        return status_t {status_code_t::invalid_argument_k, "Buffer is too small"};
    std::memcpy(destination.data(), &value, sizeof(value_type_));
    return {};
}

/** Copies the object representation of @p value out of @p source. */
template <trivially_serializable_concept value_type_>
status_t load(value_type_& value, bytes_view_t source) noexcept {
    if (source.size() < sizeof(value_type_)) return status_t {status_code_t::malformed_k, "Corrupted data"};
    std::memcpy(&value, source.data(), sizeof(value_type_));
    return {};
}

/** The bytes a serializable value reports. */
template <serializable_concept value_type_>
std::size_t storage_requirements(value_type_ const& value) noexcept {
    return value.storage_requirements();
}

/** Saves a serializable value through its own member. */
template <serializable_concept value_type_>
status_t save(value_type_ const& value, bytes_span_t destination) noexcept {
    return value.save(destination);
}

/** Loads a serializable value through its own member. */
template <serializable_concept value_type_>
status_t load(value_type_& value, bytes_view_t source) noexcept {
    return value.load(source);
}

/** The bytes of a string: its 16-bit length, then its characters. */
inline std::size_t storage_requirements(string_t const& text) noexcept { return sizeof(std::uint16_t) + text.size(); }

/** Saves the 16-bit length of @p text, then its characters. */
inline status_t save(string_t const& text, bytes_span_t destination) noexcept {
    if (text.size() > std::numeric_limits<std::uint16_t>::max())
        return status_t {status_code_t::capacity_exhausted_k, "Data is too big"};
    if (destination.size() < storage_requirements(text))
        return status_t {status_code_t::invalid_argument_k, "Buffer is too small"};
    std::uint16_t const length = static_cast<std::uint16_t>(text.size());
    std::memcpy(destination.data(), &length, sizeof(length));
    std::memcpy(destination.data() + sizeof(length), text.data(), length);
    return {};
}

/** Loads a string saved as its 16-bit length, then its characters. */
inline status_t load(string_t& text, bytes_view_t source) noexcept {
    std::uint16_t length = 0;
    if (status_t const status = load(length, source); status.failed()) return status;
    if (source.size() - sizeof(length) < length) return status_t {status_code_t::malformed_k, "Corrupted data"};
    char const* const characters = reinterpret_cast<char const*>(source.data() + sizeof(length));
    if (!text.try_assign(string_view_t {characters, length}))
        return status_t {status_code_t::out_of_memory_k, "No room for a loaded string"};
    return {};
}

/** The bytes of an array: its 64-bit count, then every element. */
template <typename element_type_, typename allocator_type_>
std::size_t storage_requirements(st::basic_vector<element_type_, allocator_type_> const& elements) noexcept {
    std::size_t bytes = sizeof(std::uint64_t);
    for (element_type_ const& element : elements) bytes += storage_requirements(element);
    return bytes;
}

/** Saves the 64-bit count of @p elements, then every element back to back. */
template <typename element_type_, typename allocator_type_>
status_t save(st::basic_vector<element_type_, allocator_type_> const& elements, bytes_span_t destination) noexcept {
    std::uint64_t const count = elements.size();
    if (status_t const status = save(count, destination); status.failed()) return status;
    std::size_t offset = sizeof(count);
    for (element_type_ const& element : elements) {
        if (status_t const status = save(element, destination.subspan(offset)); status.failed()) return status;
        offset += storage_requirements(element);
    }
    return {};
}

/** Loads an array saved as its 64-bit count, then every element back to back. */
template <typename element_type_, typename allocator_type_>
status_t load(st::basic_vector<element_type_, allocator_type_>& elements, bytes_view_t source) noexcept {
    std::uint64_t count = 0;
    if (status_t const status = load(count, source); status.failed()) return status;
    std::size_t offset = sizeof(count);
    // Every element takes at least one byte, so a larger count is corrupt.
    if (count > source.size() - offset) return status_t {status_code_t::malformed_k, "Corrupted data"};
    if (st::status_t const status = elements.resize(count); status != st::status_t::success_k) return to_status(status);
    for (element_type_& element : elements) {
        if (status_t const status = load(element, source.subspan(offset)); status.failed()) return status;
        offset += storage_requirements(element);
    }
    return {};
}

/** The bytes of @p fields stored back to back. */
template <typename... field_types_>
std::size_t fields_storage_requirements(field_types_ const&... fields) noexcept {
    return (std::size_t {0} + ... + storage_requirements(fields));
}

/** Saves no fields. */
inline status_t save_fields(bytes_span_t) noexcept { return {}; }

/** Saves @p field, then @p fields, back to back into @p destination. */
template <typename field_type_, typename... field_types_>
status_t save_fields(bytes_span_t destination, field_type_ const& field, field_types_ const&... fields) noexcept {
    if (status_t const status = save(field, destination); status.failed()) return status;
    return save_fields(destination.subspan(storage_requirements(field)), fields...);
}

/** Loads no fields. */
inline status_t load_fields(bytes_view_t) noexcept { return {}; }

/** Loads @p field, then @p fields, stored back to back in @p source. */
template <typename field_type_, typename... field_types_>
status_t load_fields(bytes_view_t source, field_type_& field, field_types_&... fields) noexcept {
    if (status_t const status = load(field, source); status.failed()) return status;
    return load_fields(source.subspan(storage_requirements(field)), fields...);
}

#pragma endregion Serialization

} // namespace unum::ustore::nvme
