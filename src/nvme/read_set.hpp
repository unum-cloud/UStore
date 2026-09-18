/**
 *  @file src/nvme/read_set.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Stored keys a transaction watches for key conflicts, with the time and purpose of each watch.
 */
#pragma once

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint8_t`

#include <algorithm> // `std::min`
#include <new>       // placement `new`

#include <smashtable/basic_hash_table.hpp> // `st::hash_map`, `st::assume_reserved_t`

#include "nvme/resources.hpp" // `resources_shared_ptr_t`
#include "nvme/types.hpp"     // `time_point_t`, `constrained_allocator`

namespace unum::ustore::nvme {

/** When a transaction checks its watched keys for conflicts. */
enum class watch_schedule_t : std::uint8_t {

    /** Ignores key collisions. */
    never_k,

    /** Checks in place, at the moment of the call. */
    now_k,

    /** Checks between the transaction start and its commit. */
    on_commit_k,

    /** Checks between the operation and the commit. */
    on_commit_since_operation_k,

    /** Schedule used when none is specified. */
    default_k = on_commit_k,
};

/** Why a transaction watches a key, which a second watch of the same key widens. */
enum class watch_purpose_t : std::uint8_t {

    /** The transaction read the key. */
    read_k = 0x01,

    /** The transaction wrote the key. */
    write_k = 0x02,

    /** The transaction tracks the key without reading or writing it. */
    track_k = 0x04,

    /** Every purpose, which a count with no filter asks for. */
    all_k = read_k | write_k | track_k,
};

/** The purposes of either watch. */
constexpr watch_purpose_t operator|(watch_purpose_t first, watch_purpose_t second) noexcept {
    return static_cast<watch_purpose_t>(static_cast<std::uint8_t>(first) | static_cast<std::uint8_t>(second));
}

/** The purposes both watches share. */
constexpr watch_purpose_t operator&(watch_purpose_t first, watch_purpose_t second) noexcept {
    return static_cast<watch_purpose_t>(static_cast<std::uint8_t>(first) & static_cast<std::uint8_t>(second));
}

/**
 *  @brief The keys one transaction watches, each with the instant it was first watched and why.
 *
 *  Watching the same key twice keeps the earlier instant and widens the purpose, so a key read and
 *  then written conflicts on both counts from the earlier of the two moments.
 */
template <typename stored_key_>
class read_set {
  public:
    using stored_key_t = stored_key_;

    /** When a key was first watched, and why. */
    struct watch_t {

        /** The earliest instant any watch of this key was taken at. */
        time_point_t start_time;

        /** Every purpose the watches of this key were taken for. */
        watch_purpose_t purpose;
    };

    using stored_keys_t =
        st::hash_map<stored_key_t, watch_t, st::hash<stored_key_t>, st::equal_to_t, constrained_allocator<std::byte>>;

    /** A set charging @p resources for the keys it holds. */
    explicit read_set(resources_shared_ptr_t const& resources) noexcept
        : stored_keys({}, {}, constrained_allocator<std::byte> {resources->memory_constraint}) {}

    /** Whether the transaction watches nothing. */
    bool empty() const noexcept { return stored_keys.empty(); }

    /** The watched keys taken for every purpose in @p purpose. */
    std::size_t count(watch_purpose_t purpose = watch_purpose_t::all_k) const noexcept {
        if (purpose == watch_purpose_t::all_k) return stored_keys.size();
        std::size_t matching = 0;
        for (auto const& entry : stored_keys) matching += (entry.mapped.purpose & purpose) == purpose;
        return matching;
    }

    /** Makes room for @p count more keys, so a following @c add takes no allocation. */
    status_t reserve_more(std::size_t count) noexcept {
        if (stored_keys.reserve_more(count) == stored_keys_t::reserve_result_t::failed_k)
            return {status_code_t::out_of_memory_k, "No room for the keys a transaction watches"};
        return {};
    }

    /**
     *  @brief Watches @p stored_key from @p start_time for @p purpose, in room @c reserve_more took.
     *  @param[in] stored_key The key the transaction watches.
     *  @param[in] start_time The instant this watch was taken at, which only lowers a key's instant.
     *  @param[in] purpose Why this watch was taken, which only widens a key's purpose.
     */
    void add(stored_key_t stored_key, time_point_t start_time, watch_purpose_t purpose,
             st::assume_reserved_t) noexcept {
        auto insert = [&](auto& unused_slot) noexcept {
            new (&unused_slot.key_ref()) stored_key_t {stored_key};
            new (&unused_slot.value_ref()) watch_t {start_time, purpose};
        };
        auto update = [&](auto& equal_slot) noexcept {
            watch_t& watch = equal_slot.value_ref();
            watch.start_time = std::min(start_time, watch.start_time);
            watch.purpose = watch.purpose | purpose;
        };
        [[maybe_unused]] auto const stored = stored_keys.probe_to_upsert(stored_key, insert, update,
                                                                         st::assume_reserved_t {});
    }

    /** Drops every watch, for a transaction starting over. */
    void clear() noexcept { stored_keys.clear(); }

    /** The watched keys, charged to the budget the set was made with. */
    stored_keys_t stored_keys;
};

} // namespace unum::ustore::nvme
