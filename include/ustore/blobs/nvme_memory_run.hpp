/**
 *  @file include/ustore/blobs/nvme_memory_run.hpp
 *  @author Ash Vardanian
 *  @date October 18, 2026
 *  @brief One commit's deltas for one collection, sealed into sorted columns and stamped once into an immutable run.
 *
 *  A committer seals its private deltas off every shared path, sorting them and copying their bytes, and the leader
 *  that orders the commit stamps the sealed columns into a @c memory_run, which no one changes afterwards. A delta
 *  is a key, a kind and the bytes between two neighbouring offsets, so a removal and an empty value differ by kind and
 *  neither needs a sentinel. The stamps and the kinds are packed columns, which spend no bits at all on a run of one
 *  commit's writes.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm> // `std::max`, `std::min`, `std::sort`
#include <memory>    // `std::allocator_traits`
#include <span>      // `std::span`
#include <utility>   // `std::move`

#include <smashtable/basic_vector.hpp>           // `st::basic_vector`
#include <smashtable/immutable_packed_array.hpp> // `st::immutable_packed_array`
#include <smashtable/shared.hpp>                 // `st::default_allocator`, `st::expected`

#include "ustore/blobs/nvme_compaction.hpp" // `compaction_stamps_t`, `keeps_older_version`
#include "ustore/blobs/nvme_key_slot.hpp"   // `transaction_stamp_t`, `stored_value_offset_t`, `storage_limits_t`
#include "ustore/utilities/folds.hpp"       // `delta_kind_t`, `delta_view_t`, `fold_concept`, `fold_delta_pair`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocator`
#include "ustore/utilities/types.hpp"       // `byte_t`, `expected`, `status_t`

namespace unum::ustore::blobs {

using utilities::delta_kind_t;
using utilities::delta_view_t;

/** One delta a transaction hands over for sealing: the key it is filed under, its kind and its bytes. */
template <typename stored_key_type_>
struct keyed_delta {

    /** The key the delta is filed under. */
    stored_key_type_ stored_key {};

    /** What the delta does to the key's value, and the bytes it carries, empty for a removal. */
    delta_view_t delta;
};

/** One delta as a merge carries it over: the key, the stamp of the commit that wrote it, its kind and its bytes. */
template <typename stored_key_type_>
struct stamped_delta {

    /** The key the version is filed under. */
    stored_key_type_ stored_key {};

    /** The stamp of the commit that wrote it. */
    transaction_stamp_t stamp = unwritten_stamp_k;

    /** What it does to the key's value, and the bytes it carries, owned by the run it is carried from. */
    delta_view_t delta;
};

/**
 *  @brief The deltas one transaction wrote to one collection, sorted by key and copied into columns, not yet stamped.
 *
 *  Sealing runs on the committing thread before the commit is ordered, so the sort and the copy never sit on a path
 *  another commit waits on.
 */
template <typename stored_key_type_, typename allocator_type_ = st::default_allocator<byte_t>>
class sealed_deltas {
  public:
    using stored_key_t = stored_key_type_;
    using allocator_t = allocator_type_;
    using keyed_delta_t = keyed_delta<stored_key_t>;

    /** The kind of every delta, packed from the least kind present, so a run of writes alone spends no bits. */
    using kinds_t =
        st::immutable_packed_array<std::uint8_t,
                                   typename std::allocator_traits<allocator_t>::template rebind_alloc<std::uint64_t>>;

    sealed_deltas() noexcept = default;
    sealed_deltas(sealed_deltas&&) noexcept = default;
    sealed_deltas& operator=(sealed_deltas&&) noexcept = default;
    sealed_deltas(sealed_deltas const&) = delete;
    sealed_deltas& operator=(sealed_deltas const&) = delete;

    /**
     *  @brief Sorts @p deltas by key in place and copies them into columns on @p allocator.
     *  @retval invalid_argument_k No deltas, two deltas of one key, or a removal carrying bytes.
     *  @retval capacity_exhausted_k More bytes than one sorted run file holds, which every offset is bounded by.
     *  @retval out_of_memory_k A column could not be allocated.
     */
    static expected<sealed_deltas> seal(std::span<keyed_delta_t> deltas, allocator_t allocator = {}) noexcept {
        if (deltas.empty()) return status_t {status_code_t::invalid_argument_k, "A seal takes at least one delta"};
        std::sort(deltas.begin(), deltas.end(), [](keyed_delta_t const& first, keyed_delta_t const& second) noexcept {
            return first.stored_key < second.stored_key;
        });
        std::uint64_t total_bytes = 0;
        std::uint8_t least_kind = static_cast<std::uint8_t>(deltas[0].delta.kind);
        std::uint8_t greatest_kind = least_kind;
        for (std::size_t index = 0; index != deltas.size(); ++index) {
            delta_view_t const& delta = deltas[index].delta;
            if (index != 0 && deltas[index - 1].stored_key == deltas[index].stored_key)
                return status_t {status_code_t::invalid_argument_k, "A seal takes one delta per key"};
            if (delta.kind == delta_kind_t::remove_k && !delta.bytes.empty())
                return status_t {status_code_t::invalid_argument_k, "A removal carries no bytes"};
            total_bytes += delta.bytes.size();
            least_kind = std::min(least_kind, static_cast<std::uint8_t>(delta.kind));
            greatest_kind = std::max(greatest_kind, static_cast<std::uint8_t>(delta.kind));
        }
        // A memory run flushes into one sorted run file, whose length bounds every offset either holds.
        if (total_bytes > storage_limits_t::sorted_run_file_max_size_k)
            return status_t {status_code_t::capacity_exhausted_k, "The sealed deltas outgrow one run file"};
        return columns_of_<keyed_delta_t>(deltas, total_bytes, least_kind, greatest_kind, allocator);
    }

    /**
     *  @brief Copies @p versions into columns on @p allocator, in the order given, which a merge of runs hands over
     *      in key order with the versions of one key newest first.
     *  @retval out_of_memory_k A column could not be allocated.
     */
    static expected<sealed_deltas> merged(std::span<stamped_delta<stored_key_t> const> versions,
                                          allocator_t allocator = {}) noexcept {
        std::uint64_t total_bytes = 0;
        std::uint8_t least_kind = versions.empty() ? 0 : static_cast<std::uint8_t>(versions[0].delta.kind);
        std::uint8_t greatest_kind = least_kind;
        for (stamped_delta<stored_key_t> const& version : versions) {
            total_bytes += version.delta.bytes.size();
            least_kind = std::min(least_kind, static_cast<std::uint8_t>(version.delta.kind));
            greatest_kind = std::max(greatest_kind, static_cast<std::uint8_t>(version.delta.kind));
        }
        return columns_of_(versions, total_bytes, least_kind, greatest_kind, allocator);
    }

    /** Deltas sealed, at most one per key. */
    [[nodiscard]] std::size_t size() const noexcept { return stored_keys_.size(); }

    /** Bytes every delta's value takes together. */
    [[nodiscard]] std::size_t values_bytes() const noexcept { return values_.size(); }

    /** The key at sorted @p position. */
    [[nodiscard]] stored_key_t const& stored_key(std::size_t position) const noexcept { return stored_keys_[position]; }

    /** The delta at sorted @p position: its kind and the bytes it carries. */
    [[nodiscard]] delta_view_t delta(std::size_t position) const noexcept {
        std::size_t const begin = offsets_[position];
        std::size_t const end = offsets_[position + 1];
        return delta_view_t {static_cast<delta_kind_t>(kinds_[position]),
                             std::span<byte_t const>(values_.data() + begin, end - begin)};
    }

    /** The first sorted position whose key is not below @p stored_key, or @c size when every key is. */
    [[nodiscard]] std::size_t lower_bound(stored_key_t const& stored_key) const noexcept {
        std::size_t low = 0;
        std::size_t high = stored_keys_.size();
        while (low != high) {
            std::size_t const middle = low + (high - low) / 2;
            if (stored_keys_[middle] < stored_key) low = middle + 1;
            else high = middle;
        }
        return low;
    }

  private:
    using stored_keys_t =
        st::basic_vector<stored_key_t,
                         typename std::allocator_traits<allocator_t>::template rebind_alloc<stored_key_t>>;
    using offsets_t =
        st::basic_vector<stored_value_offset_t,
                         typename std::allocator_traits<allocator_t>::template rebind_alloc<stored_value_offset_t>>;
    using values_t =
        st::basic_vector<byte_t, typename std::allocator_traits<allocator_t>::template rebind_alloc<byte_t>>;

    explicit sealed_deltas(allocator_t const& allocator) noexcept
        : stored_keys_(typename stored_keys_t::allocator_t(allocator)),
          offsets_(typename offsets_t::allocator_t(allocator)), values_(typename values_t::allocator_t(allocator)) {}

    /** Copies the keys, the bytes and the kinds of @p deltas, in their order, into sealed columns. */
    template <typename delta_type_>
    static expected<sealed_deltas> columns_of_(std::span<delta_type_ const> deltas, std::uint64_t total_bytes,
                                               std::uint8_t least_kind, std::uint8_t greatest_kind,
                                               allocator_t const& allocator) noexcept {
        sealed_deltas sealed(allocator);
        if (sealed.stored_keys_.resize(deltas.size()) != st::status_t::success_k ||
            sealed.offsets_.resize(deltas.size() + 1) != st::status_t::success_k ||
            sealed.values_.resize(static_cast<std::size_t>(total_bytes)) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room to seal the deltas"};
        stored_value_offset_t offset = 0;
        for (std::size_t index = 0; index != deltas.size(); ++index) {
            std::span<byte_t const> const bytes = deltas[index].delta.bytes;
            sealed.stored_keys_[index] = deltas[index].stored_key;
            sealed.offsets_[index] = offset;
            if (!bytes.empty()) std::memcpy(sealed.values_.data() + offset, bytes.data(), bytes.size());
            offset += static_cast<stored_value_offset_t>(bytes.size());
        }
        sealed.offsets_[deltas.size()] = offset;

        std::uint8_t pulled = 0;
        auto fill = [&](std::size_t position) noexcept {
            pulled = static_cast<std::uint8_t>(deltas[position].delta.kind);
            return st::expected<std::span<std::uint8_t const>>(std::span<std::uint8_t const>(&pulled, 1),
                                                               st::status_t::success_k);
        };
        st::expected<kinds_t> kinds = kinds_t::make(deltas.size(), least_kind, greatest_kind, fill,
                                                    typename kinds_t::allocator_t(allocator));
        if (!kinds) return status_t {status_code_t::out_of_memory_k, "No room to pack the kinds of sealed deltas"};
        sealed.kinds_ = std::move(*kinds);
        return sealed;
    }

    /** The keys in ascending order. */
    stored_keys_t stored_keys_;

    /** One offset per delta and one past the last, so a delta's bytes run from its offset to the next one. */
    offsets_t offsets_;

    /** Every delta's bytes back to back, in key order. */
    values_t values_;

    /** Every delta's @c delta_kind_t, packed. */
    kinds_t kinds_;
};

/**
 *  @brief One commit's deltas for one collection, stamped, ordered by key, and never changed once published.
 *
 *  Versions of one key sit newest first, which a merge of several runs relies on; a run sealed from one commit holds
 *  one version per key. The stamps column is based at the oldest stamp, so a run of one commit spends no bits on it.
 */
template <typename stored_key_type_, typename allocator_type_ = st::default_allocator<byte_t>>
class memory_run {
  public:
    using stored_key_t = stored_key_type_;
    using allocator_t = allocator_type_;
    using sealed_deltas_t = sealed_deltas<stored_key_t, allocator_t>;
    using stamps_t =
        st::immutable_packed_array<std::uint64_t,
                                   typename std::allocator_traits<allocator_t>::template rebind_alloc<std::uint64_t>>;

    memory_run() noexcept = default;
    memory_run(memory_run&&) noexcept = default;
    memory_run& operator=(memory_run&&) noexcept = default;
    memory_run(memory_run const&) = delete;
    memory_run& operator=(memory_run const&) = delete;

    /**
     *  @brief Stamps every delta of @p deltas with the commit's @p stamp, which only the leader ordering it draws.
     *  @retval invalid_argument_k An unwritten stamp, which no commit carries.
     *
     *  Every delta carries the one stamp, whose column packs into no words, so nothing is allocated and a stamp
     *  drawn for a commit never fails to land.
     */
    static expected<memory_run> stamped(sealed_deltas_t&& deltas, transaction_stamp_t stamp,
                                        allocator_t allocator = {}) noexcept {
        if (stamp == unwritten_stamp_k)
            return status_t {status_code_t::invalid_argument_k, "No commit carries the unwritten stamp"};
        st::expected<stamps_t> stamps = stamps_t::make(deltas.size(), stamp, typename stamps_t::allocator_t(allocator));
        if (!stamps) return status_t {status_code_t::out_of_memory_k, "No room to stamp the sealed deltas"};
        memory_run run;
        run.deltas_ = std::move(deltas);
        run.stamps_ = std::move(*stamps);
        run.newest_stamp_ = stamp;
        return run;
    }

    /**
     *  @brief The run one merge makes of @p versions, which come in key order with the versions of a key newest first,
     *      for runs of one tier it replaces.
     *  @param[in] oldest_stamp The oldest stamp of the runs it replaces, which the run keeps covering where that
     *      version did not survive, so neighbouring runs still never overlap and the durable stamp stays below every
     *      commit the run holds.
     *  @param[in] tier The tier of the runs it replaces, which the run is one above.
     */
    static expected<memory_run> merged(std::span<stamped_delta<stored_key_t> const> versions,
                                       transaction_stamp_t oldest_stamp, std::uint32_t tier,
                                       allocator_t allocator = {}) noexcept {
        transaction_stamp_t newest_stamp = oldest_stamp;
        for (stamped_delta<stored_key_t> const& version : versions)
            newest_stamp = std::max(newest_stamp, version.stamp);
        auto [deltas, merged_status] = sealed_deltas_t::merged(versions, allocator);
        if (merged_status.failed()) return merged_status;
        transaction_stamp_t pulled = unwritten_stamp_k;
        auto fill = [&](std::size_t position) noexcept {
            pulled = versions[position].stamp;
            return st::expected<std::span<transaction_stamp_t const>>(std::span<transaction_stamp_t const>(&pulled, 1),
                                                                      st::status_t::success_k);
        };
        st::expected<stamps_t> stamps = stamps_t::make(versions.size(), oldest_stamp, newest_stamp, fill,
                                                       typename stamps_t::allocator_t(allocator));
        if (!stamps) return status_t {status_code_t::out_of_memory_k, "No room to stamp a merged run"};
        memory_run run;
        run.deltas_ = std::move(deltas);
        run.stamps_ = std::move(*stamps);
        run.newest_stamp_ = newest_stamp;
        run.tier_ = tier + 1;
        return run;
    }

    /** How many merges built the run: tier zero for one commit's, and one above the runs a merge replaced. */
    [[nodiscard]] std::uint32_t tier() const noexcept { return tier_; }

    /** Deltas the run holds, every version counted. */
    [[nodiscard]] std::size_t size() const noexcept { return deltas_.size(); }

    /** Bytes every delta's value takes together. */
    [[nodiscard]] std::size_t values_bytes() const noexcept { return deltas_.values_bytes(); }

    /** The key at sorted @p position. */
    [[nodiscard]] stored_key_t const& stored_key(std::size_t position) const noexcept {
        return deltas_.stored_key(position);
    }

    /** The delta at sorted @p position: its kind and the bytes it carries. */
    [[nodiscard]] delta_view_t delta(std::size_t position) const noexcept { return deltas_.delta(position); }

    /** The stamp of the commit that wrote the delta at sorted @p position. */
    [[nodiscard]] transaction_stamp_t stamp(std::size_t position) const noexcept { return stamps_[position]; }

    /** The first sorted position whose key is not below @p stored_key, or @c size when every key is. */
    [[nodiscard]] std::size_t lower_bound(stored_key_t const& stored_key) const noexcept {
        return deltas_.lower_bound(stored_key);
    }

    /** The newest stamp any delta carries. */
    [[nodiscard]] transaction_stamp_t newest_stamp() const noexcept { return newest_stamp_; }

    /** The oldest stamp any delta carries, which the stamps column is based at. */
    [[nodiscard]] transaction_stamp_t oldest_stamp() const noexcept { return stamps_.base(); }

  private:

    /** The sorted keys, offsets, bytes and kinds. */
    sealed_deltas_t deltas_;

    /** The stamp of every delta in sorted order. */
    stamps_t stamps_;

    /** The newest stamp in @c stamps_, which a check of what changed since a stamp skips the run by. */
    transaction_stamp_t newest_stamp_ = unwritten_stamp_k;

    /** How many merges built the run. */
    std::uint32_t tier_ = 0;
};

/**
 *  @brief Gathers every version @p runs hold into @p versions, in key order with a key's versions newest first,
 *      dropping each older version no reader of @p survival tells apart from the newer one kept before it.
 *
 *  Such a version under a kept change folds into it through @p fold instead, keeping the change's stamp: a write or a
 *  removal below makes it a write, and a change below composes with it, its bytes landing in @p arena. The versions
 *  view the runs' keys and bytes, so the runs outlive them until a merged run or a flushed one copies them.
 */
template <typename run_type_, utilities::fold_concept fold_type_, typename versions_type_>
status_t kept_versions(std::span<run_type_ const* const> runs, compaction_stamps_t const& survival,
                       fold_type_ const& fold, versions_type_& versions,
                       utilities::arena_allocator<byte_t>& arena) noexcept {
    using version_t = stamped_delta<typename run_type_::stored_key_t>;
    std::size_t count = 0;
    for (run_type_ const* run : runs) count += run->size();
    if (versions.reserve(count) != st::status_t::success_k)
        return {status_code_t::out_of_memory_k, "A merge could not hold its versions"};
    for (run_type_ const* run : runs)
        for (std::size_t position = 0; position != run->size(); ++position) [[maybe_unused]]
            st::status_t const placed = versions.push_back(
                st::assume_reserved, version_t {run->stored_key(position), run->stamp(position), run->delta(position)});
    std::sort(versions.begin(), versions.end(), [](version_t const& first, version_t const& second) noexcept {
        if (first.stored_key < second.stored_key) return true;
        if (second.stored_key < first.stored_key) return false;
        return first.stamp > second.stamp;
    });
    std::size_t kept = 0;
    for (std::size_t index = 0; index != versions.size(); ++index) {
        bool const shadowed = kept != 0 && versions[kept - 1].stored_key == versions[index].stored_key &&
                              !keeps_older_version(survival, versions[kept - 1].stamp, versions[index].stamp);
        if (!shadowed) {
            versions[kept++] = versions[index];
            continue;
        }
        delta_view_t& newer = versions[kept - 1].delta;
        if (newer.kind != delta_kind_t::change_k) continue;
        auto [folded, fold_status] = utilities::fold_delta_pair(fold, newer, versions[index].delta, arena);
        if (fold_status.failed()) return fold_status;
        newer = folded;
    }
    while (versions.size() != kept) versions.pop_back();
    return {};
}

} // namespace unum::ustore::blobs
