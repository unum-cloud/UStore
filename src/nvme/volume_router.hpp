/**
 *  @file src/nvme/volume_router.hpp
 *  @author Ash Vardanian
 *  @date September 19, 2026
 *  @brief The volumes one database spans, and the placement that decides which of them a blob lands on.
 *
 *  A machine has one volume per data directory rather than one per database, so the layer above the
 *  volume is a placement decision and nothing else: it holds the mounted volumes, answers which of
 *  them a blob of a given length should go to, and stamps every handle it hands back with the volume
 *  that owns the bytes. Resolution stays a one-off - a name is turned into a @c blob_t once, and
 *  every block read afterwards carries the target it found, so nothing here sits on the read path.
 *
 *  The router keeps no lock and no claim. One volume's catalog and free map are mutated by one
 *  thread at a time exactly as they were before it, and a router that spun on a word would promise a
 *  safety the volume underneath it does not have.
 */

#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <concepts> // `std::same_as`
#include <utility>  // `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`

#include "nvme/catalog.hpp" // `blob_kind_t`, `blob_name_t`
#include "nvme/io_lane.hpp" // `io_lane_t`
#include "nvme/volume.hpp"  // `blob_t`, `volume_id_t`

namespace unum::ustore::nvme {

#pragma region Contract

/** What a router asks of a volume: the names it resolves, and the free space it reports. */
template <typename volume_type_>
concept extent_volume_concept = requires(volume_type_& volume, volume_type_ const& constant, io_lane_t& lane,
                                         blob_name_t name, blob_kind_t kind, std::uint64_t length) {
    { volume.create(name, kind, length) } -> std::same_as<expected<blob_t>>;
    { constant.find(name) } -> std::same_as<expected<blob_t>>;
    { volume.drop(lane, name) } -> std::same_as<status_t>;
    { constant.names(name) } -> std::same_as<bool>;
    { constant.free_bytes() } -> std::same_as<std::uint64_t>;
    { constant.largest_free_bytes() } -> std::same_as<std::uint64_t>;
};

#pragma endregion Contract

#pragma region Router

/**
 *  @brief Every mounted volume of one database, and the free-space placement that spreads blobs over them.
 *
 *  The template is on the volume rather than on a variant of volumes, because a mount already knows
 *  which transport it opened and a virtual call per placement would buy nothing; a runtime-dispatched
 *  router wraps this one rather than replacing it.
 */
template <typename volume_type_>
    requires extent_volume_concept<volume_type_>
class basic_volume_router {
  public:

    basic_volume_router() noexcept = default;
    basic_volume_router(basic_volume_router const&) = delete;
    basic_volume_router& operator=(basic_volume_router const&) = delete;
    basic_volume_router(basic_volume_router&&) noexcept = default;
    basic_volume_router& operator=(basic_volume_router&&) noexcept = default;

    /**
     *  @brief An empty router with room for @p volumes_limit volumes, which a mount fills one at a time.
     *  @param[in] volumes_limit Volumes this router will ever hold, which is one per data directory.
     *
     *  @note The room is taken here and never grown, because an LSM tree holds a pointer to the volume
     *      it was mounted on; a later adoption that moved the volumes would leave every tree
     *      addressing freed memory.
     */
    static expected<basic_volume_router> make(std::size_t volumes_limit = 1) noexcept {
        if (volumes_limit == 0 || volumes_limit >= static_cast<std::size_t>(volume_id_t::unmounted_k))
            return status_t {status_code_t::invalid_argument_k, "A router names one volume to an identifier's worth"};
        basic_volume_router router;
        if (router.volumes_.reserve(volumes_limit) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "The router could not reserve its volumes"};
        router.limit_ = volumes_limit;
        return router;
    }

    /**
     *  @brief Takes @p volume in under the next identifier, which every blob placed on it carries.
     *  @param[in] volume The mounted volume, moved in and owned by the router from here on.
     */
    status_t adopt(volume_type_&& volume) noexcept {
        // Refused rather than grown, since growing would move volumes their trees still point at.
        if (volumes_.size() >= limit_)
            return {status_code_t::capacity_exhausted_k, "The router reserved room for fewer volumes than that"};
        if (volumes_.push_back(std::move(volume)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "The router could not track another volume"};
        return {};
    }

    /**
     *  @brief The volume with the most free space that a blob of @p length still fits in.
     *  @param[in] length Bytes the blob asks for, before the volume rounds it to its granularity.
     *  @return The volume, or @c status_code_t::capacity_exhausted_k when none has a range that long.
     */
    expected<volume_id_t> optimal_volume(std::uint64_t length) const noexcept {
        std::size_t chosen = volumes_.size();
        std::uint64_t chosen_free = 0;
        for (std::size_t index = 0; index != volumes_.size(); ++index) {
            volume_type_ const& candidate = volumes_[index];
            if (candidate.largest_free_bytes() < length) continue;
            std::uint64_t const free = candidate.free_bytes();
            if (chosen != volumes_.size() && free <= chosen_free) continue;
            chosen = index;
            chosen_free = free;
        }
        if (chosen == volumes_.size())
            return status_t {status_code_t::capacity_exhausted_k, "No mounted volume fits a blob that long"};
        return static_cast<volume_id_t>(chosen);
    }

    /**
     *  @brief Places @p name by free space and reserves @p length bytes for it on the volume that won.
     *  @param[in] name The bytes the blob is named by, which no other volume of the router may hold.
     *  @param[in] kind What the blob is, which is how @c for_each enumerates it later.
     *  @param[in] length Bytes the caller will write, rounded up by the volume that takes it.
     *  @return The blob, stamped with the volume it was placed on, which the caller seals there.
     */
    expected<blob_t> create(blob_name_t name, blob_kind_t kind, std::uint64_t length) noexcept {
        for (std::size_t index = 0; index != volumes_.size(); ++index)
            if (volumes_[index].names(name))
                return status_t {status_code_t::already_exists_k, "A volume of this router names that blob"};

        expected<volume_id_t> const placed = optimal_volume(length);
        if (!placed) return placed.status();
        expected<blob_t> created = volumes_[static_cast<std::size_t>(*placed)].create(name, kind, length);
        if (!created) return created.status();
        created->volume = *placed;
        return created;
    }

    /** The blob @p name resolves to on whichever volume holds it, stamped with that volume. */
    expected<blob_t> find(blob_name_t name) const noexcept {
        for (std::size_t index = 0; index != volumes_.size(); ++index) {
            expected<blob_t> found = volumes_[index].find(name);
            if (!found) continue;
            found->volume = static_cast<volume_id_t>(index);
            return found;
        }
        return status_t {status_code_t::not_found_k, "No volume of this router holds that blob"};
    }

    /** Forgets @p name on the one volume that holds it, which is the only one the drop reaches. */
    status_t drop(io_lane_t& lane, blob_name_t name) noexcept {
        for (std::size_t index = 0; index != volumes_.size(); ++index)
            if (volumes_[index].names(name)) return volumes_[index].drop(lane, name);
        return {status_code_t::not_found_k, "No volume of this router holds that blob"};
    }

    /** Calls @p visitor with the name and the stamped blob of every entry of @p kind, volume by volume. */
    template <typename visitor_type_>
    void for_each(blob_kind_t kind, visitor_type_&& visitor) const noexcept {
        for (std::size_t index = 0; index != volumes_.size(); ++index)
            volumes_[index].for_each(kind, [&](blob_name_t name, blob_t blob) noexcept {
                blob.volume = static_cast<volume_id_t>(index);
                visitor(name, blob);
            });
    }

    /** The volume @p id names, which a caller reaches to seal the blob it has just written. */
    [[nodiscard]] volume_type_& volume(volume_id_t id) noexcept {
        assert(static_cast<std::size_t>(id) < volumes_.size() && "A router resolves only the volumes it adopted");
        return volumes_[static_cast<std::size_t>(id)];
    }

    /** Volumes the router has adopted, whose identifiers run from zero to one below it. */
    [[nodiscard]] std::size_t size() const noexcept { return volumes_.size(); }

  private:

    /** Volumes this router reserved room for, fixed when it was made and never grown. */
    std::size_t limit_ = 0;

    /** Every adopted volume, in the order they were adopted, which is the order of their identifiers. */
    st::basic_vector<volume_type_> volumes_;
};

#pragma endregion Router

} // namespace unum::ustore::nvme
