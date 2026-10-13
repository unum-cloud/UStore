/**
 *  @file cpp/ustore/types.hpp
 *  @author Ash Vardanian
 *  @date July 4, 2022
 *  @brief Locations, edges, value views, arenas, stamps, receivers, executors and progress callables shared by every
 *      C++ wrapper.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `SIZE_MAX`, `std::int64_t`, `std::uint64_t`
#include <cstring> // `std::strlen`

#include <algorithm>   // `std::ranges::equal`
#include <bit>         // `std::bit_floor`
#include <compare>     // `std::strong_ordering`
#include <concepts>    // `std::same_as`, `std::integral`, `std::unsigned_integral`
#include <functional>  // `std::hash`
#include <limits>      // `std::numeric_limits`
#include <ranges>      // `std::ranges::contiguous_range`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::remove_cvref_t`, `std::is_scalar_v`, `std::invoke_result_t`, `std::type_identity_t`
#include <utility>     // `std::exchange`, `std::swap`

#include "ustore/status.hpp" // `expected`, `status_t`
#include "ustore/ustore.h"

namespace ashvardanian::forkunion {}
namespace ashvardanian::smashtable {}
namespace ashvardanian::stringzilla {}

#pragma region Identifiers

// The C structs live in the global namespace, so their comparisons live there too, where lookup finds them

/** Orders identifiers as their bytes, which is how every engine stores and walks them. */
constexpr std::strong_ordering operator<=>(ustore_uuid_t const& first, ustore_uuid_t const& second) noexcept {
    return std::lexicographical_compare_three_way(first.bytes, first.bytes + 16, second.bytes, second.bytes + 16);
}
constexpr bool operator==(ustore_uuid_t const& first, ustore_uuid_t const& second) noexcept {
    return (first <=> second) == 0;
}
constexpr std::strong_ordering operator<=>(ustore_key_t const& first, ustore_key_t const& second) noexcept {
    return first.uuid <=> second.uuid;
}
constexpr bool operator==(ustore_key_t const& first, ustore_key_t const& second) noexcept {
    return first.uuid == second.uuid;
}
constexpr std::strong_ordering operator<=>(ustore_collection_id_t const& first,
                                           ustore_collection_id_t const& second) noexcept {
    return first.uuid <=> second.uuid;
}
constexpr bool operator==(ustore_collection_id_t const& first, ustore_collection_id_t const& second) noexcept {
    return first.uuid == second.uuid;
}

template <>
struct std::hash<ustore_uuid_t> {
    std::size_t operator()(ustore_uuid_t const& uuid) const noexcept {
        std::uint64_t halves[2];
        std::memcpy(halves, uuid.bytes, sizeof(halves));
        return std::hash<std::uint64_t> {}(halves[0] ^ (halves[1] * 0x9E3779B97F4A7C15ull));
    }
};

template <>
struct std::hash<ustore_key_t> {
    std::size_t operator()(ustore_key_t const& key) const noexcept { return std::hash<ustore_uuid_t> {}(key.uuid); }
};

template <>
struct std::hash<ustore_collection_id_t> {
    std::size_t operator()(ustore_collection_id_t const& id) const noexcept {
        return std::hash<ustore_uuid_t> {}(id.uuid);
    }
};

#pragma endregion Identifiers

namespace unum::ustore {

namespace fu = ashvardanian::forkunion;
namespace st = ashvardanian::smashtable;
namespace sz = ashvardanian::stringzilla;

#pragma region Identifier Arithmetic

/** The UUID spelling @p integer big-endian in its last eight bytes, so UUIDs made from integers order as the
 *  integers, exactly as Python's `uuid.UUID(int=integer)` does. */
constexpr ustore_uuid_t uuid_from_integer(std::uint64_t integer) noexcept {
    ustore_uuid_t uuid {};
    for (unsigned index = 0; index != 8; ++index)
        uuid.bytes[15 - index] = static_cast<std::uint8_t>(integer >> (8 * index));
    return uuid;
}

/** The integer @p uuid spells, which only a UUID with its first eight bytes zero has. */
inline expected<std::uint64_t> uuid_to_integer(ustore_uuid_t const& uuid) noexcept {
    std::uint64_t integer = 0;
    for (unsigned index = 0; index != 8; ++index)
        if (uuid.bytes[index] != 0) return status_t {status_code_t::invalid_argument_k, "The UUID exceeds 64 bits"};
    for (unsigned index = 8; index != 16; ++index) integer = (integer << 8) | uuid.bytes[index];
    return integer;
}

/** The key spelling @p integer, as @c uuid_from_integer does. */
constexpr ustore_key_t key_from_integer(std::uint64_t integer) noexcept { return {uuid_from_integer(integer)}; }

/** The first key in their order, the nil UUID, where a walk of a whole collection starts. */
inline constexpr ustore_key_t smallest_key_k {};

/** The last key in their order, every bit set, which @c ustore_key_unknown_k spells too. */
inline constexpr ustore_key_t largest_key_k {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}};

/** The key right after @p key in their order, where a scan resuming past @p key starts; the last key has none and
 *  wraps to the first. */
constexpr ustore_key_t key_after(ustore_key_t key) noexcept {
    for (unsigned index = 16; index != 0; --index)
        if (++key.uuid.bytes[index - 1] != 0) break;
    return key;
}

#pragma endregion Identifier Arithmetic

#pragma region Identifier Text

/** Characters the canonical `8-4-4-4-12` text of a UUID takes, dashes included. */
inline constexpr std::size_t uuid_text_length_k = 36;

/** Writes the canonical lowercase `8-4-4-4-12` text of @p uuid into @p text, without a terminator. */
constexpr void uuid_to_text(ustore_uuid_t const& uuid, std::span<char, uuid_text_length_k> text) noexcept {
    constexpr char digits_k[] = "0123456789abcdef";
    std::size_t position = 0;
    for (unsigned index = 0; index != 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) text[position++] = '-';
        text[position++] = digits_k[uuid.bytes[index] >> 4];
        text[position++] = digits_k[uuid.bytes[index] & 0x0F];
    }
}

/** Reads @p text as canonical UUID text in either case, or as a non-negative decimal integer, which spells as
 *  @c uuid_from_integer does. */
inline expected<ustore_uuid_t> uuid_from_text(std::string_view text) noexcept {
    status_t const malformed {status_code_t::invalid_argument_k, "Neither UUID text nor a non-negative integer"};
    ustore_uuid_t uuid {};
    if (text.size() == uuid_text_length_k) {
        std::size_t position = 0;
        for (unsigned index = 0; index != 16; ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10) {
                if (text[position] != '-') return malformed;
                ++position;
            }
            unsigned byte = 0;
            for (unsigned half = 0; half != 2; ++half, ++position) {
                char const digit = text[position];
                unsigned const value = digit >= '0' && digit <= '9'   ? unsigned(digit - '0')
                                       : digit >= 'a' && digit <= 'f' ? unsigned(digit - 'a' + 10)
                                       : digit >= 'A' && digit <= 'F' ? unsigned(digit - 'A' + 10)
                                                                      : 16u;
                if (value == 16) return malformed;
                byte = byte * 16 + value;
            }
            uuid.bytes[index] = static_cast<std::uint8_t>(byte);
        }
        return uuid;
    }
    if (text.empty() || text.size() > 20) return malformed;
    std::uint64_t integer = 0;
    for (char const digit : text) {
        if (digit < '0' || digit > '9') return malformed;
        std::uint64_t const scaled = integer * 10 + unsigned(digit - '0');
        if (scaled / 10 != integer) return malformed;
        integer = scaled;
    }
    return uuid_from_integer(integer);
}

#pragma endregion Identifier Text

using f64_t = double;
using f32_t = float;

using i64_t = std::int64_t;
using i32_t = std::int32_t;
using i16_t = std::int16_t;
using i8_t = std::int8_t;

using u64_t = std::uint64_t;
using u32_t = std::uint32_t;
using u16_t = std::uint16_t;
using u8_t = std::uint8_t;

/** A single byte of a stored value, kept distinct from characters and integers. */
using byte_t = std::byte;

#pragma region Locations

/** A key inside a collection, laid out so a strided pointer can address either member. */
struct collection_key_t {

    /** Collection owning the key. */
    ustore_collection_id_t collection {ustore_collection_main_k};

    /** Key inside the collection. */
    ustore_key_t key {};

    collection_key_t() = default;
    explicit collection_key_t(ustore_key_t key) noexcept : key(key) {}
    collection_key_t(ustore_collection_id_t collection, ustore_key_t key) noexcept : collection(collection), key(key) {}

    /** The same key addressed inside @p other_collection. */
    collection_key_t in(ustore_collection_id_t other_collection) const noexcept { return {other_collection, key}; }

    friend bool operator==(collection_key_t const&, collection_key_t const&) = default;
    friend std::strong_ordering operator<=>(collection_key_t const&, collection_key_t const&) = default;
};

/** A key inside a collection, optionally narrowed down to one field of its document. */
struct collection_key_field_t {

    /** Collection owning the key. */
    ustore_collection_id_t collection {};

    /** Key inside the collection. */
    ustore_key_t key {ustore_key_unknown_k};

    /** Path of the document field, or an empty one for the whole value. */
    std::string_view field {};

    collection_key_field_t() = default;
    collection_key_field_t(ustore_collection_id_t collection, ustore_key_t key, std::string_view field = {}) noexcept
        : collection(collection), key(key), field(field) {}
    collection_key_field_t(ustore_key_t key) noexcept : collection(ustore_collection_main_k), key(key) {}
    explicit collection_key_field_t(ustore_key_t key, std::string_view field) noexcept
        : collection(ustore_collection_main_k), key(key), field(field) {}
};

/** A location that names the collection it lives in. */
template <typename location_>
concept has_collection_concept = requires(location_ const& location) { location.collection; };

/** A location that narrows its value down to one document field. */
template <typename location_>
concept has_field_concept = requires(location_ const& location) { location.field; };

/** A single key, bare or with its collection, that widens into a @c collection_key_field_t. */
template <typename location_>
concept key_location_concept = std::same_as<std::remove_cvref_t<location_>, ustore_key_t> ||
                               std::same_as<std::remove_cvref_t<location_>, collection_key_t>;

#pragma endregion Locations

#pragma region Graph Elements

/** A graph edge, or a relationship in DBMS terms. */
struct edge_t {

    /** Vertex the edge leaves. */
    ustore_key_t source;

    /** Vertex the edge enters. */
    ustore_key_t target;

    /** Identifier of the edge itself. */
    ustore_key_t id {ustore_edge_id_default_k};

    friend bool operator==(edge_t const&, edge_t const&) = default;
};

/** One side of an edge as a vertex stores it, kept sorted inside the vertex's adjacency list. */
struct neighborship_t {

    /** Vertex on the other end of the edge. */
    ustore_key_t neighbor_id {};

    /** Identifier of the edge. */
    ustore_key_t edge_id {};

    friend bool operator==(neighborship_t const&, neighborship_t const&) = default;
    friend std::strong_ordering operator<=>(neighborship_t const&, neighborship_t const&) = default;
};

#pragma endregion Graph Elements

#pragma region Variable Length Views

/** The length a missing value carries, which no stored value reaches, so a view tells it apart from an empty one. */
inline constexpr ustore_length_t missing_length_k = std::numeric_limits<ustore_length_t>::max();

/** A view of a stored value that tells a missing value apart from an empty one. */
class value_view_t {

    ustore_byte_t const* pointer_ = nullptr;
    ustore_length_t length_ = missing_length_k;

  public:
    using value_type = byte_t;

    value_view_t() = default;
    value_view_t(ustore_byte_t const* pointer, ustore_length_t length) noexcept : pointer_(pointer), length_(length) {}

    /** Views the bytes of @p bytes, a value that is always present. */
    value_view_t(ustore_bytes_view_t bytes) noexcept : pointer_(bytes.data), length_(bytes.length) {}

    value_view_t(byte_t const* begin, byte_t const* end) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(begin)), length_(static_cast<ustore_length_t>(end - begin)) {}

    value_view_t(byte_t const* begin, std::size_t length) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(begin)), length_(static_cast<ustore_length_t>(length)) {}

    value_view_t(char const* string) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(string)),
          length_(static_cast<ustore_length_t>(std::strlen(string))) {}

    value_view_t(char const* string, std::size_t length) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(string)), length_(static_cast<ustore_length_t>(length)) {}

    template <typename char_type_, typename traits_type_>
    value_view_t(std::basic_string_view<char_type_, traits_type_> view) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(view.data())),
          length_(static_cast<ustore_length_t>(view.size() * sizeof(char_type_))) {}

    value_view_t(std::string_view view) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(view.data())),
          length_(static_cast<ustore_length_t>(view.size())) {}

    /** An empty value that is present, unlike a default-constructed missing one. */
    static value_view_t make_empty() noexcept { return value_view_t {nullptr, nullptr}; }

    operator bool() const noexcept { return length_ != missing_length_k; }
    std::size_t size() const noexcept { return length_ == missing_length_k ? 0 : length_; }
    byte_t const* data() const noexcept {
        return length_ != missing_length_k ? reinterpret_cast<byte_t const*>(pointer_) : nullptr;
    }

    char const* c_str() const noexcept { return reinterpret_cast<char const*>(pointer_); }
    byte_t const* begin() const noexcept { return data(); }
    byte_t const* end() const noexcept { return data() + size(); }
    bool empty() const noexcept { return !size(); }
    operator std::string_view() const noexcept { return {c_str(), size()}; }

    /** The run of bytes the C ABI reads, where a missing value passes as an empty one. */
    ustore_bytes_view_t bytes() const noexcept { return {pointer_, size()}; }

    bool operator==(value_view_t other) const noexcept { return std::ranges::equal(*this, other); }
};

/** The bytes of @p text, which the C ABI reads without a terminator. */
inline ustore_bytes_view_t bytes_of(std::string_view text) noexcept {
    return {reinterpret_cast<ustore_byte_t const*>(text.data()), text.size()};
}

/** The text @p bytes spell. */
inline std::string_view string_of(ustore_bytes_view_t bytes) noexcept {
    return {reinterpret_cast<char const*>(bytes.data), static_cast<std::size_t>(bytes.length)};
}

/** Views the bytes of a contiguous container. */
template <std::ranges::contiguous_range container_>
    requires std::ranges::sized_range<container_>
value_view_t value_view(container_&& container) noexcept {
    using element_t = std::ranges::range_value_t<container_>;
    return {reinterpret_cast<byte_t const*>(std::ranges::data(container)),
            std::ranges::size(container) * sizeof(element_t)};
}

#pragma endregion Variable Length Views

#pragma region Memory Management

/** Owns an arena made where an executor computes, which the C ABI fills with results, freeing it on destruction. */
class arena_t {

    ustore_arena_t arena_ = nullptr;

  public:
    arena_t() noexcept = default;
    arena_t(arena_t const&) = delete;
    arena_t& operator=(arena_t const&) = delete;
    ~arena_t() noexcept { ustore_arena_free(arena_); }

    arena_t(arena_t&& other) noexcept : arena_(std::exchange(other.arena_, nullptr)) {}

    arena_t& operator=(arena_t&& other) noexcept {
        std::swap(arena_, other.arena_);
        return *this;
    }

    /** Makes an arena where @p executor computes, or on host pages for calls on the calling thread. */
    static expected<arena_t> make(ustore_executor_t executor = nullptr) noexcept {
        arena_t made;
        ustore_outcome_t outcome {};
        ustore_arena_init_t init {.arena = &made.arena_, .executor = executor, .outcome = &outcome};
        ustore_arena_init(&init);
        if (status_t const status {outcome}; status.failed()) return status;
        return made;
    }

    operator ustore_arena_t() const noexcept { return arena_; }
};

/** A view of what one call wrote into an arena made for that call alone, which it frees on destruction, as shorthand
 *  with no place for an arena answers. It moves but never copies, and @c view slices off the plain view. */
template <typename view_type_>
class owned : public view_type_ {
    arena_t arena_;

  public:
    owned() noexcept = default;
    owned(view_type_ const& view, arena_t arena) noexcept : view_type_(view), arena_(std::move(arena)) {}

    view_type_ const& view() const noexcept { return *this; }
};

using owned_value_t = owned<value_view_t>;

/** What shorthand answers for a call answering @p answer_type_: a scalar copied out of its one-off arena, or a view
 *  owning that arena. */
template <typename answer_type_>
using one_off_t = std::conditional_t<std::is_scalar_v<answer_type_>, answer_type_, owned<answer_type_>>;

/** Runs @p verb on an arena made for it alone where @p executor computes, as shorthand with no place for an arena
 *  does, answering a scalar copied out of that arena or a view that owns it. */
template <typename verb_type_>
auto in_one_off_arena(ustore_executor_t executor, verb_type_&& verb) noexcept {
    using answer_t = typename std::invoke_result_t<verb_type_&, arena_t&>::value_type;
    using result_t = expected<one_off_t<answer_t>>;
    auto [arena, arena_status] = arena_t::make(executor);
    if (arena_status.failed()) return result_t {arena_status};
    auto [answer, answer_status] = verb(arena);
    if (answer_status.failed()) return result_t {answer_status};
    if constexpr (std::is_scalar_v<answer_t>) return result_t {answer};
    else return result_t {owned<answer_t> {answer, std::move(arena)}};
}

#pragma endregion Memory Management

#pragma region Receivers

/** The stamp a transaction or a snapshot reads at, which a commit validates against. */
enum class read_stamp_t : ustore_stamp_t {};

/** A commit's position in commit order, visible to every snapshot reading at or above it. */
enum class commit_stamp_t : ustore_stamp_t {};

/** What every call made through one receiver names before its inputs: the database, the executor that runs it, and
 *  the transaction or snapshot its reads see and its writes go through. */
struct receiver_t {

    /** An open database. */
    ustore_database_t database = nullptr;

    /** Executor that runs the calls, where @c nullptr runs them on the calling thread. */
    ustore_executor_t executor = nullptr;

    /** Transaction the writes go through and the reads see, or @c nullptr for the head. */
    ustore_transaction_t transaction = nullptr;

    /** Snapshot the reads see when no transaction is named, or @c nullptr for the head. */
    ustore_snapshot_t snapshot = nullptr;

    /** The view a read sees, which inside a transaction records the keys it reads unless @p watch says otherwise. */
    ustore_view_t view(ustore_watch_t watch) const noexcept {
        return transaction ? ustore_transaction_view(transaction, watch) : ustore_snapshot_view(snapshot);
    }

    /** The same receiver, running its calls on @p runner. */
    receiver_t on(ustore_executor_t runner) const noexcept { return {database, runner, transaction, snapshot}; }
};

/** Owns an executor, the threads or I/O rings that run calls on any number of databases, and frees it once drained.
 *  It never copies, and outlives every call submitted to it. */
class executor_handle_t {
    ustore_executor_t executor_ = nullptr;

    /** Creates the executor behind @p init with @p options, whose outputs this call fills in. */
    template <typename options_type_>
    static expected<executor_handle_t> make_(void (*init)(options_type_*), options_type_ options) noexcept {
        executor_handle_t executor;
        ustore_outcome_t outcome {};
        options.executor = &executor.executor_;
        options.outcome = &outcome;
        init(&options);
        if (status_t const status {outcome}; status.failed()) return status;
        return executor;
    }

  public:
    executor_handle_t() noexcept = default;
    executor_handle_t(executor_handle_t const&) = delete;
    executor_handle_t& operator=(executor_handle_t const&) = delete;
    executor_handle_t(executor_handle_t&& other) noexcept : executor_(std::exchange(other.executor_, nullptr)) {}
    executor_handle_t& operator=(executor_handle_t&& other) noexcept {
        std::swap(executor_, other.executor_);
        return *this;
    }
    ~executor_handle_t() noexcept { ustore_executor_free(executor_); }

    operator ustore_executor_t() const noexcept { return executor_; }

    /** Spawns host threads for the RAM, RocksDB and LevelDB engines. */
    static expected<executor_handle_t> for_ram(ustore_executor_for_ram_t options = {}) noexcept {
        return make_(ustore_executor_for_ram, options);
    }

    /** Borrows the ForkUnion pool @p options name for the RAM, RocksDB and LevelDB engines. */
    static expected<executor_handle_t> on_pool(ustore_executor_on_pool_t options) noexcept {
        return make_(ustore_executor_on_pool, options);
    }

    /** Spawns threads for the NVMe engine, each owning its own I/O ring. */
    static expected<executor_handle_t> for_nvme(ustore_executor_for_nvme_t options = {}) noexcept {
        return make_(ustore_executor_for_nvme, options);
    }

    /** Waits for every call submitted before the drain, leaving the threads in @p mood whenever they go idle. */
    status_t drain(ustore_mood_t mood = ustore_mood_parked_k) noexcept {
        ustore_outcome_t outcome {};
        ustore_executor_drain_t drain {.executor = executor_, .outcome = &outcome, .mood = mood};
        ustore_executor_drain(&drain);
        return status_t {outcome};
    }
};

/** Whether @p progress_type_ can watch a long call: given the units finished and the units in total, it answers
 *  whether the call carries on, and never throws. */
template <typename progress_type_>
concept progress_concept = requires(progress_type_& progress, ustore_size_t finished, ustore_size_t total) {
    { progress(finished, total) } noexcept -> std::same_as<ustore_continuation_t>;
};

/** A callable lent to a long call as the C ABI receives it, a callback and the state it reads, or neither for a call
 *  nothing watches. The call borrows the callable until it is final. */
class progress_arg_t {

    template <progress_concept progress_type_>
    static ustore_continuation_t report_(ustore_size_t finished, ustore_size_t total, void* state) noexcept {
        return (*static_cast<progress_type_*>(state))(finished, total);
    }

  public:

    /** Reports the advance of the call, or @c nullptr for a call nothing watches. */
    ustore_progress_callback_t callback = nullptr;

    /** The callable that @c callback calls. */
    void* state = nullptr;

    progress_arg_t() noexcept = default;

    /** Lends @p progress, which answers @c ustore_cancel_k to stop the call. */
    template <progress_concept progress_type_>
    progress_arg_t(progress_type_& progress) noexcept : callback(&report_<progress_type_>), state(&progress) {}
};

#pragma endregion Receivers

#pragma region Adapters

/**
 *  Mixes the hash of @p value into @p seed.
 *  @see https://www.boost.org/doc/libs/1_37_0/doc/html/hash/reference.html#boost.hash_combine
 */
template <typename hashable_>
void hash_combine(std::size_t& seed, hashable_ const& value) noexcept {
    std::hash<hashable_> hasher;
    seed ^= hasher(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

/** Hashes a key together with its collection. */
struct collection_key_hash_t {
    std::size_t operator()(collection_key_t const& location) const noexcept {
        std::size_t result = SIZE_MAX;
        hash_combine(result, location.key);
        hash_combine(result, location.collection);
        return result;
    }
};

/** Hashes an edge by both its ends and its identifier. */
struct edge_hash_t {
    std::size_t operator()(edge_t const& edge) const noexcept {
        std::size_t result = SIZE_MAX;
        hash_combine(result, edge.source);
        hash_combine(result, edge.target);
        hash_combine(result, edge.id);
        return result;
    }
};

/** The role the other end of an edge plays. */
inline ustore_vertex_role_t invert(ustore_vertex_role_t role) noexcept {
    switch (role) {
    case ustore_vertex_role_source_k: return ustore_vertex_role_target_k;
    case ustore_vertex_role_target_k: return ustore_vertex_role_source_k;
    case ustore_vertex_role_any_k: return ustore_vertex_role_unknown_k;
    case ustore_vertex_role_unknown_k: return ustore_vertex_role_any_k;
    }
    return ustore_vertex_role_unknown_k;
}

constexpr std::size_t bits_in_byte_k = 8;

/** The power of two just above the highest set bit of @p value. */
inline std::size_t next_power_of_two(std::size_t value) noexcept { return std::bit_floor(value) << 1; }

/** @p value divided by @p divisor and rounded up, without the overflow `value + divisor - 1` risks near the limit. */
template <std::unsigned_integral integer_>
constexpr integer_ divide_round_up(integer_ value, std::type_identity_t<integer_> divisor) noexcept {
    return static_cast<integer_>(value / divisor + (value % divisor != 0));
}

/** The greatest multiple of @p multiple at or below @p value. */
template <std::unsigned_integral integer_>
constexpr integer_ round_down_to_multiple(integer_ value, std::type_identity_t<integer_> multiple) noexcept {
    return static_cast<integer_>(value - value % multiple);
}

/** The least multiple of @p multiple at or above @p value, saturating at the greatest multiple @p integer_ holds. */
template <std::unsigned_integral integer_>
constexpr integer_ round_up_to_multiple(integer_ value, std::type_identity_t<integer_> multiple) noexcept {
    integer_ const below = round_down_to_multiple(value, multiple);
    if (below == value || below > std::numeric_limits<integer_>::max() - multiple) return below;
    return static_cast<integer_>(below + multiple);
}

/** The document field type that stores @p element_. */
template <typename element_>
constexpr ustore_docs_field_type_t docs_field_type() noexcept {
    if constexpr (std::is_same_v<element_, bool>) return ustore_docs_field_type_bool_k;
    if constexpr (std::is_same_v<element_, f64_t>) return ustore_docs_field_type_f64_k;
    if constexpr (std::is_same_v<element_, f32_t>) return ustore_docs_field_type_f32_k;
    if constexpr (std::is_same_v<element_, i64_t>) return ustore_docs_field_type_i64_k;
    if constexpr (std::is_same_v<element_, i32_t>) return ustore_docs_field_type_i32_k;
    if constexpr (std::is_same_v<element_, i16_t>) return ustore_docs_field_type_i16_k;
    if constexpr (std::is_same_v<element_, i8_t>) return ustore_docs_field_type_i8_k;
    if constexpr (std::is_same_v<element_, u64_t>) return ustore_docs_field_type_u64_k;
    if constexpr (std::is_same_v<element_, u32_t>) return ustore_docs_field_type_u32_k;
    if constexpr (std::is_same_v<element_, u16_t>) return ustore_docs_field_type_u16_k;
    if constexpr (std::is_same_v<element_, u8_t>) return ustore_docs_field_type_u8_k;
    if constexpr (std::is_same_v<element_, value_view_t>) return ustore_docs_field_type_bin_k;
    if constexpr (std::is_same_v<element_, std::string_view> || std::is_same_v<element_, char const*>)
        return ustore_docs_field_type_str_k;
    return ustore_docs_field_type_json_k;
}

#pragma endregion Adapters

} // namespace unum::ustore

template <>
struct std::hash<unum::ustore::collection_key_t> : unum::ustore::collection_key_hash_t {};
