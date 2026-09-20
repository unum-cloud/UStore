/**
 *  @file src/nvme/database_snapshot.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief One level set per collection, held together so a whole database answers at one moment.
 *
 *  A database snapshot is the per-collection snapshot repeated, and nothing more: taking one flushes
 *  every write buffer and then counts a reader into every tree, so no run any collection names can be
 *  retired while the snapshot lives. Writes that land afterwards go to level sets this one never saw.
 *
 *  The snapshot borrows the collections it names, exactly as a collection snapshot borrows its tree,
 *  so a collection must not be dropped while a snapshot still names it.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <span>    // `std::span`
#include <utility> // `std::move`
#include <variant> // `std::variant`, `std::visit`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/collection_snapshot.hpp" // `collection_snapshot`, `select_result_t`
#include "nvme/io_channel.hpp"          // `io_channel_t`
#include "nvme/key_slot.hpp"            // `key_slot`
#include "nvme/stored_value.hpp"        // `stored_value_size_t`
#include "nvme/types.hpp"               // `collection_numeric_id_t`, `value_layout_t`, `string_t`

namespace unum::ustore::nvme {

#pragma region Snapshot

/**
 *  @brief The level set of every collection of one database, held together against publication.
 *  @tparam stored_key_ The key every collection of the database is addressed by.
 *  @tparam volume_type_ The volume every run of every held collection lives on.
 */
template <typename stored_key_, typename volume_type_>
class database_snapshot {
  public:

#pragma region Types

    /** The key every collection of the database is addressed by. */
    using stored_key_t = stored_key_;

    /** The volume every run of every held collection lives on. */
    using volume_t = volume_type_;

    /** A collection named by its number or by its name, which is how every call addresses one. */
    using any_collection_id_t = std::variant<collection_numeric_id_t, collection_alphanumeric_id_t>;

    /** One held level set of a collection whose values follow @p value_layout_. */
    template <value_layout_t value_layout_>
    using collection_snapshot_of = collection_snapshot<key_slot<stored_key_t, value_layout_>, volume_t>;

    /** One held level set, of whichever value layout its collection was created with. */
    using any_collection_snapshot_t = std::variant<collection_snapshot_of<value_layout_t::inline_1_byte_k>,
                                                   collection_snapshot_of<value_layout_t::inline_4_bytes_k>,
                                                   collection_snapshot_of<value_layout_t::inline_8_bytes_k>,
                                                   collection_snapshot_of<value_layout_t::inline_16_bytes_k>,
                                                   collection_snapshot_of<value_layout_t::inline_32_bytes_k>,
                                                   collection_snapshot_of<value_layout_t::inline_64_bytes_k>,
                                                   collection_snapshot_of<value_layout_t::fixed_width_k>,
                                                   collection_snapshot_of<value_layout_t::variable_width_k>>;

    /** One collection of the snapshot: the names it answers to, and the level set it holds. */
    struct held_collection_t {

        /** The number the database addresses the collection by. */
        collection_numeric_id_t numeric_id = collection_invalid_numeric_id_k;

        /** The name the database addresses the collection by. */
        string_t name;

        /** The level set held, which nothing may retire while this snapshot lives. */
        any_collection_snapshot_t levels;
    };

#pragma endregion Types

#pragma region Construction

    database_snapshot() noexcept = default;
    database_snapshot(database_snapshot const&) = delete;
    database_snapshot& operator=(database_snapshot const&) = delete;
    database_snapshot(database_snapshot&&) noexcept = default;
    database_snapshot& operator=(database_snapshot&&) noexcept = default;

    /** Makes room for @p count collections, so no hold is taken that a later one cannot be tracked beside. */
    status_t reserve(std::size_t count) noexcept {
        if (held_.reserve(count) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A snapshot could not track that many collections"};
        return {};
    }

    /**
     *  @brief Takes @p levels in under @p numeric_id and @p name, in room @c reserve took.
     *  @param[in] numeric_id The number the database addresses the collection by.
     *  @param[in] name The name the database addresses the collection by.
     *  @param[in] levels The level set already held, moved in and released when the snapshot dies.
     */
    status_t hold(collection_numeric_id_t numeric_id, string_view_t name, any_collection_snapshot_t&& levels) noexcept {
        held_collection_t taken;
        taken.numeric_id = numeric_id;
        if (!taken.name.try_assign(name))
            return {status_code_t::out_of_memory_k, "A snapshot could not hold a collection's name"};
        taken.levels = std::move(levels);
        if (held_.push_back(std::move(taken)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A snapshot could not track another collection"};
        return {};
    }

#pragma endregion Construction

#pragma region Shape

    /** Collections this snapshot holds a level set of. */
    [[nodiscard]] std::size_t size() const noexcept { return held_.size(); }

    /** Whether @p id names a collection this snapshot holds. */
    [[nodiscard]] bool contains(any_collection_id_t const& id) const noexcept { return held_at_(id) != nullptr; }

    /** The collection at @p index, in the order the snapshot took them. */
    [[nodiscard]] held_collection_t const& at(std::size_t index) const noexcept { return held_[index]; }

    /** Key slots every level of every held collection carries together. */
    [[nodiscard]] std::uint64_t population_count() const noexcept {
        std::uint64_t counted = 0;
        for (std::size_t index = 0; index != held_.size(); ++index)
            counted += std::visit([](auto const& levels) noexcept { return levels.population_count(); },
                                  held_[index].levels);
        return counted;
    }

#pragma endregion Shape

#pragma region Lookups

    /**
     *  @brief Whether the held levels of @p id carry @p stored_key, and how many bytes its value takes.
     *  @param[in] id The collection to look in, named by its number or by its name.
     *  @param[in] stored_key The key to look for.
     *  @param[out] size Bytes the value takes, which is zero for anything but @c success_k.
     */
    [[nodiscard]] select_result_t find(any_collection_id_t const& id, stored_key_t stored_key,
                                       stored_value_size_t& size) const noexcept {
        size = 0;
        held_collection_t const* const found = held_at_(id);
        if (!found) return select_result_t::nothing_k;
        return std::visit([&](auto const& levels) noexcept { return levels.find(stored_key, size); }, found->levels);
    }

    /**
     *  @brief Copies the value @p id held for @p stored_key into @p into, which needs only the value's bytes.
     *  @param[inout] channel The channel the read is submitted and completed on, held by the calling thread.
     *  @param[in] id The collection to read from, named by its number or by its name.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     */
    status_t select(io_channel_t& channel, any_collection_id_t const& id, stored_key_t stored_key,
                    std::span<byte_t> into, stored_value_size_t& size) const noexcept {
        size = 0;
        held_collection_t const* const found = held_at_(id);
        if (!found) return {status_code_t::not_found_k, "This snapshot holds no collection of that name"};
        return std::visit([&](auto const& levels) noexcept { return levels.select(channel, stored_key, into, size); },
                          found->levels);
    }

#pragma endregion Lookups

  private:

#pragma region Private Methods

    /** The collection @p id names, or null when the snapshot holds none. */
    [[nodiscard]] held_collection_t const* held_at_(any_collection_id_t const& id) const noexcept {
        if (collection_numeric_id_t const* const numbered = std::get_if<collection_numeric_id_t>(&id)) {
            for (std::size_t index = 0; index != held_.size(); ++index)
                if (held_[index].numeric_id == *numbered) return &held_[index];
            return nullptr;
        }
        collection_alphanumeric_id_t const& named = std::get<collection_alphanumeric_id_t>(id);
        for (std::size_t index = 0; index != held_.size(); ++index)
            if (held_[index].name == named) return &held_[index];
        return nullptr;
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** Every collection the snapshot holds, in the order it took them. */
    st::basic_vector<held_collection_t> held_;

#pragma endregion Private Members
};

#pragma endregion Snapshot

} // namespace unum::ustore::nvme
