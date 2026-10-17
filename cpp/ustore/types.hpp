/**
 *  @file cpp/ustore/types.hpp
 *  @author Ash Vardanian
 *  @date July 4, 2022
 *  @brief Locations, arenas, stamps, receivers, executors and progress callables shared by every C++ wrapper, beside
 *      the identifiers, edges and value views it re-exports from `ustore/utilities/types.hpp`.
 */

#pragma once

#include <concepts>    // `std::same_as`
#include <ranges>      // `std::ranges::contiguous_range`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::remove_cvref_t`, `std::is_scalar_v`, `std::invoke_result_t`, `std::conditional_t`
#include <utility>     // `std::exchange`, `std::move`, `std::swap`

#include "ustore/status.hpp"          // `expected`, `status_t`
#include "ustore/ustore.h"            // `ustore_arena_t`, `ustore_executor_t`, `ustore_progress_callback_t`
#include "ustore/utilities/types.hpp" // `value_view_t`, `collection_key_t`, `edge_t`, `uuid_from_integer`

namespace unum::ustore {

using utilities::bits_in_byte_k;
using utilities::byte_t;
using utilities::bytes_of;
using utilities::collection_key_hash_t;
using utilities::collection_key_t;
using utilities::divide_round_up;
using utilities::edge_hash_t;
using utilities::edge_t;
using utilities::f32_t;
using utilities::f64_t;
using utilities::hash_combine;
using utilities::i16_t;
using utilities::i32_t;
using utilities::i64_t;
using utilities::i8_t;
using utilities::invert;
using utilities::key_after;
using utilities::key_from_integer;
using utilities::largest_key_k;
using utilities::missing_length_k;
using utilities::neighborship_t;
using utilities::next_power_of_two;
using utilities::round_down_to_multiple;
using utilities::round_up_to_multiple;
using utilities::smallest_key_k;
using utilities::string_of;
using utilities::u16_t;
using utilities::u32_t;
using utilities::u64_t;
using utilities::u8_t;
using utilities::uuid_from_integer;
using utilities::uuid_from_text;
using utilities::uuid_text_length_k;
using utilities::uuid_to_integer;
using utilities::uuid_to_text;
using utilities::value_view_t;

#pragma region Locations

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

#pragma region Variable Length Views

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
