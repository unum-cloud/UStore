/**
 *  @file include/ustore/blobs/nvme_memory_run.hpp
 *  @author Ash Vardanian
 *  @date October 18, 2026
 *  @brief One commit's deltas for one collection, sealed into sorted columns and stamped once into an immutable run.
 *
 *  A committer seals its private deltas off every shared path, sorting them and copying their
 *  bytes, and the leader that orders the commit stamps the sealed columns into a @c memory_run,
 *  which no one changes afterwards. A delta is a key, a kind and the bytes between two neighbouring
 *  offsets, so a removal and an empty value differ by kind and neither needs a sentinel. The stamps
 *  and the kinds are packed columns, which spend no bits at all on a run of one commit's writes.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm> // `std::max`, `std::min`
#include <memory>    // `std::allocator_traits`
#include <span>      // `std::span`
#include <utility>   // `std::move`

#include <smashtable/basic_vector.hpp>           // `st::vector`
#include <smashtable/immutable_packed_array.hpp> // `st::immutable_packed_array`
#include <smashtable/immutable_splus_tree.hpp>   // `st::immutable_splus_tree`
#include <smashtable/shared.hpp>                 // `st::default_allocator`, `st::expected`

#include "ustore/blobs/nvme_compaction.hpp" // `compaction_stamps_t`, `keeps_older_version`
#include "ustore/blobs/nvme_key_slot.hpp"   // `transaction_stamp_t`, `stored_value_offset_t`, `storage_limits_t`
#include "ustore/utilities/algorithm.hpp"   // `sort_by_stored_key`, `loser_tree`
#include "ustore/utilities/folds.hpp"       // `delta_kind_t`, `delta_view_t`, `fold_concept`, `fold_delta_pair`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocator`
#include "ustore/utilities/types.hpp"       // `byte_t`, `expected`, `status_t`

namespace unum::ustore::blobs {

using utilities::delta_kind_t;
using utilities::delta_view_t;

/** One delta a transaction hands over for sealing: the key it is filed under, its kind and its bytes. */
template <typename stored_key_type_>
struct keyed_delta {
    using stored_key_t = stored_key_type_;

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
template <typename stored_key_type_, typename allocator_type_ = st::default_allocator<byte_t>,
          typename row_kit_type_ = st::native_row_kit_t>
class sealed_deltas {
  public:
    using stored_key_t = stored_key_type_;
    using allocator_t = allocator_type_;
    using keyed_delta_t = keyed_delta<stored_key_t>;

    /** The range tombstones the deltas carry beside them, stamped with the deltas. */
    using range_tombstones_t = blobs::range_tombstones<stored_key_t, allocator_t>;

    /** The keys one range tombstone hides. */
    using range_t = range_tombstone<stored_key_t>;

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
     *  @brief Sorts @p deltas by key in place and copies them and @p ranges into columns on
     *      @p allocator; a delta inside a range lands with the commit that removes the range, so
     *      the range hides none of them.
     *
     *  @par Statuses:
     *  - @c invalid_argument_k No delta and no range, two deltas of one key, a removal carrying
     *    bytes, or a range ending at or before its start.
     *  - @c capacity_exhausted_k More bytes than one sorted run file holds, which every offset
     *    is bounded by.
     *  - @c out_of_memory_k A column could not be allocated.
     */
    static expected<sealed_deltas> seal(std::span<keyed_delta_t> deltas, std::span<range_t const> ranges = {},
                                        allocator_t allocator = {}) noexcept {
        if (deltas.empty() && ranges.empty())
            return status_t {status_code_t::invalid_argument_k, "A seal takes at least one delta or range"};
        utilities::sort_by_stored_key(deltas);
        std::uint64_t total_bytes = 0;
        std::uint8_t least_kind = deltas.empty() ? 0 : static_cast<std::uint8_t>(deltas[0].delta.kind);
        std::uint8_t greatest_kind = least_kind;
        for (std::size_t index = 0; index != deltas.size(); ++index) {
            delta_view_t const& delta = deltas[index].delta;
            if (index != 0 && deltas[index - 1].stored_key == deltas[index].stored_key)
                return status_t {status_code_t::invalid_argument_k, "A seal takes one delta per key"};
            if (delta.kind == delta_kind_t::remove_k && !delta.bytes.empty())
                return status_t {status_code_t::invalid_argument_k, "A removal carries no bytes"};
            if (delta.kind == delta_kind_t::remove_range_k)
                return status_t {status_code_t::invalid_argument_k, "A range removal seals as a range"};
            total_bytes += delta.bytes.size();
            least_kind = std::min(least_kind, static_cast<std::uint8_t>(delta.kind));
            greatest_kind = std::max(greatest_kind, static_cast<std::uint8_t>(delta.kind));
        }
        // A memory run flushes into one sorted run file, whose length bounds every offset either holds.
        if (total_bytes > storage_limits_t::sorted_run_file_max_size_k)
            return status_t {status_code_t::capacity_exhausted_k, "The sealed deltas outgrow one run file"};
        auto [sealed, sealed_status] = columns_of_<keyed_delta_t>(deltas, total_bytes, least_kind, greatest_kind,
                                                                  allocator);
        if (sealed_status.failed()) return sealed_status;
        // The commit's stamp lands in the column allocated here, so stamping allocates nothing.
        st::vector<std::uint64_t, typename std::allocator_traits<allocator_t>::template rebind_alloc<std::uint64_t>>
            unstamped {typename std::allocator_traits<allocator_t>::template rebind_alloc<std::uint64_t>(allocator)};
        if (unstamped.resize(ranges.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room to seal the range tombstones"};
        for (std::uint64_t& stamp : unstamped) stamp = unwritten_stamp_k;
        auto [tombstones, tombstones_status] = range_tombstones_t::make(
            ranges, std::span<std::uint64_t const> {unstamped}, allocator);
        if (tombstones_status.failed()) return tombstones_status;
        sealed.tombstones_ = std::move(tombstones);
        return std::move(sealed);
    }

    /**
     *  @brief Copies @p versions into columns on @p allocator, in the order given, which a merge of runs hands over
     *      in key order with the versions of one key newest first.
     *  @retval out_of_memory_k A column could not be allocated.
     */
    static expected<sealed_deltas> merged(std::span<stamped_delta<stored_key_t> const> versions,
                                          range_tombstones_t&& tombstones, allocator_t allocator = {}) noexcept {
        std::uint64_t total_bytes = 0;
        std::uint8_t least_kind = versions.empty() ? 0 : static_cast<std::uint8_t>(versions[0].delta.kind);
        std::uint8_t greatest_kind = least_kind;
        for (stamped_delta<stored_key_t> const& version : versions) {
            total_bytes += version.delta.bytes.size();
            least_kind = std::min(least_kind, static_cast<std::uint8_t>(version.delta.kind));
            greatest_kind = std::max(greatest_kind, static_cast<std::uint8_t>(version.delta.kind));
        }
        auto [sealed, sealed_status] = columns_of_(versions, total_bytes, least_kind, greatest_kind, allocator);
        if (sealed_status.failed()) return sealed_status;
        sealed.tombstones_ = std::move(tombstones);
        return std::move(sealed);
    }

    /** Deltas sealed, at most one per key. */
    [[nodiscard]] std::size_t size() const noexcept { return stored_keys_.size(); }

    /** Bytes every delta's value takes together. */
    [[nodiscard]] std::size_t values_bytes() const noexcept { return values_.size(); }

    /** The key at sorted @p position. */
    [[nodiscard]] stored_key_t stored_key(std::size_t position) const noexcept { return stored_keys_.select(position); }

    /** The delta at sorted @p position: its kind and the bytes it carries. */
    [[nodiscard]] delta_view_t delta(std::size_t position) const noexcept {
        std::size_t const begin = offsets_[position];
        std::size_t const end = offsets_[position + 1];
        return delta_view_t {static_cast<delta_kind_t>(kinds_[position]),
                             std::span<byte_t const>(values_.data() + begin, end - begin)};
    }

    /** The first sorted position whose key is not below @p stored_key, or @c size when every key is. */
    [[nodiscard]] std::size_t lower_bound(stored_key_t const& stored_key) const noexcept {
        return stored_keys_.rank(stored_key);
    }

    /** The range tombstones sealed beside the deltas. */
    [[nodiscard]] range_tombstones_t const& range_tombstones() const noexcept { return tombstones_; }

    /** Stamps every range tombstone with the commit's @p stamp, which allocates nothing. */
    void stamp_range_tombstones(transaction_stamp_t stamp) noexcept { tombstones_.assign_stamp(stamp); }

  private:
    using key_allocator_t = typename std::allocator_traits<allocator_t>::template rebind_alloc<stored_key_t>;
    using stored_keys_t =
        st::immutable_splus_tree<stored_key_t, st::keys_per_row<stored_key_t>(st::default_row_bytes_k), row_kit_type_,
                                 key_allocator_t>;
    using offsets_t =
        st::vector<stored_value_offset_t,
                   typename std::allocator_traits<allocator_t>::template rebind_alloc<stored_value_offset_t>>;
    using values_t = st::vector<byte_t, typename std::allocator_traits<allocator_t>::template rebind_alloc<byte_t>>;

    explicit sealed_deltas(allocator_t const& allocator) noexcept
        : stored_keys_(typename stored_keys_t::allocator_t(allocator)),
          offsets_(typename offsets_t::allocator_t(allocator)), values_(typename values_t::allocator_t(allocator)) {}

    /** Copies the keys, the bytes and the kinds of @p deltas, in their order, into sealed columns. */
    template <typename delta_type_>
    static expected<sealed_deltas> columns_of_(std::span<delta_type_ const> deltas, std::uint64_t total_bytes,
                                               std::uint8_t least_kind, std::uint8_t greatest_kind,
                                               allocator_t const& allocator) noexcept {
        sealed_deltas sealed(allocator);
        st::vector<stored_key_t, key_allocator_t> keys {key_allocator_t(allocator)};
        if (keys.resize(deltas.size()) != st::status_t::success_k ||
            sealed.offsets_.resize(deltas.size() + 1) != st::status_t::success_k ||
            sealed.values_.resize(static_cast<std::size_t>(total_bytes)) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room to seal the deltas"};
        stored_value_offset_t offset = 0;
        for (std::size_t index = 0; index != deltas.size(); ++index) {
            std::span<byte_t const> const bytes = deltas[index].delta.bytes;
            keys[index] = deltas[index].stored_key;
            sealed.offsets_[index] = offset;
            if (!bytes.empty()) std::memcpy(sealed.values_.data() + offset, bytes.data(), bytes.size());
            offset += static_cast<stored_value_offset_t>(bytes.size());
        }
        sealed.offsets_[deltas.size()] = offset;
        st::expected<stored_keys_t> tree = stored_keys_t::make(std::span<stored_key_t const> {keys},
                                                               key_allocator_t(allocator));
        if (!tree) return status_t {status_code_t::out_of_memory_k, "No room for the tree of the sealed keys"};
        sealed.stored_keys_ = std::move(*tree);

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

    /** The keys in ascending order, as an S+ tree every lookup descends. */
    stored_keys_t stored_keys_;

    /** One offset per delta and one past the last, so a delta's bytes run from its offset to the next one. */
    offsets_t offsets_;

    /** Every delta's bytes back to back, in key order. */
    values_t values_;

    /** Every delta's @c delta_kind_t, packed. */
    kinds_t kinds_;

    /** The range tombstones, ascending by start. */
    range_tombstones_t tombstones_;
};

/**
 *  @brief One commit's deltas for one collection, stamped, ordered by key, and never changed once published.
 *
 *  Versions of one key sit newest first, which a merge of several runs relies on; a run sealed from one commit holds
 *  one version per key. The stamps column is based at the oldest stamp, so a run of one commit spends no bits on it.
 */
template <typename stored_key_type_, typename allocator_type_ = st::default_allocator<byte_t>,
          typename row_kit_type_ = st::native_row_kit_t>
class memory_run {
  public:
    using stored_key_t = stored_key_type_;
    using allocator_t = allocator_type_;
    using sealed_deltas_t = sealed_deltas<stored_key_t, allocator_t, row_kit_type_>;
    using range_tombstones_t = typename sealed_deltas_t::range_tombstones_t;
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
        deltas.stamp_range_tombstones(stamp);
        run.deltas_ = std::move(deltas);
        run.stamps_ = std::move(*stamps);
        run.newest_stamp_ = stamp;
        return run;
    }

    /**
     *  @brief The run one merge makes of @p versions, which come in key order with the versions of a key newest first,
     *      for runs of one tier it replaces.
     *  @param[in] versions The versions to merge, in key order with the versions of a key newest first.
     *  @param[in] tombstones The range tombstones the merged run keeps.
     *  @param[in] oldest_stamp The oldest stamp of the runs it replaces, which the run keeps covering where that
     *      version did not survive, so neighbouring runs still never overlap and the durable stamp stays below every
     *      commit the run holds.
     *  @param[in] tier The tier of the runs it replaces, which the run is one above.
     *  @param[in] allocator Where the merged run's columns are allocated.
     */
    static expected<memory_run> merged(std::span<stamped_delta<stored_key_t> const> versions,
                                       range_tombstones_t&& tombstones, transaction_stamp_t oldest_stamp,
                                       std::uint32_t tier, allocator_t allocator = {}) noexcept {
        transaction_stamp_t newest_stamp = std::max(oldest_stamp, tombstones.newest_stamp());
        for (stamped_delta<stored_key_t> const& version : versions)
            newest_stamp = std::max(newest_stamp, version.stamp);
        auto [deltas, merged_status] = sealed_deltas_t::merged(versions, std::move(tombstones), allocator);
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

    /** Bytes the run charges against its store's memory bounds: its values, as its keys take a
     *  fixed width, and its range tombstones. */
    [[nodiscard]] std::uint64_t memory_bytes() const noexcept {
        return values_bytes() + deltas_.range_tombstones().bytes();
    }

    /** The key at sorted @p position. */
    [[nodiscard]] stored_key_t stored_key(std::size_t position) const noexcept { return deltas_.stored_key(position); }

    /** The delta at sorted @p position: its kind and the bytes it carries. */
    [[nodiscard]] delta_view_t delta(std::size_t position) const noexcept { return deltas_.delta(position); }

    /** The stamp of the commit that wrote the delta at sorted @p position. */
    [[nodiscard]] transaction_stamp_t stamp(std::size_t position) const noexcept { return stamps_[position]; }

    /** The first sorted position whose key is not below @p stored_key, or @c size when every key is. */
    [[nodiscard]] std::size_t lower_bound(stored_key_t const& stored_key) const noexcept {
        return deltas_.lower_bound(stored_key);
    }

    /** The range tombstones the run carries beside its deltas, every one stamped. */
    [[nodiscard]] range_tombstones_t const& range_tombstones() const noexcept { return deltas_.range_tombstones(); }

    /** The newest stamp any delta or range tombstone carries. */
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

/** Whether a version of @p stored_key stamped @p stamp hides under a range tombstone of @p runs
 *  that no reader of @p survival reads below, which the memory runs order by stamp alone, so a
 *  merge of the runs drops the version. */
template <typename run_type_>
bool hidden_by_range(std::span<run_type_ const* const> runs, typename run_type_::stored_key_t const& stored_key,
                     transaction_stamp_t stamp, compaction_stamps_t const& survival) noexcept {
    transaction_stamp_t nearest = unwritten_stamp_k;
    for (run_type_ const* run : runs) {
        transaction_stamp_t const above = run->range_tombstones().covering_stamp_above(stored_key, stamp);
        if (above != unwritten_stamp_k && (nearest == unwritten_stamp_k || above < nearest)) nearest = above;
    }
    return nearest != unwritten_stamp_k && nearest <= survival.ceiling &&
           !keeps_older_version(survival, nearest, stamp);
}

/** Every range tombstone @p runs carry, with its stamp, as one column set on @p allocator, which a
 *  merge or a flush of memory runs keeps whole since nothing they hold is durable. */
template <typename run_type_, typename allocator_type_>
expected<range_tombstones<typename run_type_::stored_key_t, allocator_type_>> gathered_range_tombstones(
    std::span<run_type_ const* const> runs, allocator_type_ const& allocator) noexcept {
    using range_tombstones_t = range_tombstones<typename run_type_::stored_key_t, allocator_type_>;
    using range_t = typename range_tombstones_t::range_t;
    std::size_t count = 0;
    for (run_type_ const* run : runs) count += run->range_tombstones().size();
    st::vector<range_t> ranges;
    st::vector<std::uint64_t> stamps;
    if (ranges.reserve(count) != st::status_t::success_k || stamps.reserve(count) != st::status_t::success_k)
        return status_t {status_code_t::out_of_memory_k, "A merge could not gather its range tombstones"};
    for (run_type_ const* run : runs)
        for (std::size_t index = 0; index != run->range_tombstones().size(); ++index) {
            [[maybe_unused]] st::status_t const ranged = ranges.push_back(
                st::assume_reserved, range_t {run->range_tombstones().range(index)});
            [[maybe_unused]] st::status_t const stamped = stamps.push_back(
                st::assume_reserved, std::uint64_t {run->range_tombstones().stamp(index)});
        }
    return range_tombstones_t::make(std::span<range_t const> {ranges}, std::span<std::uint64_t const> {stamps},
                                    allocator);
}

/**
 *  @brief Gathers every version @p runs hold into @p versions, in key order with a key's versions newest first,
 *      dropping each older version no reader of @p survival tells apart from the newer one kept before it.
 *
 *  Such a version under a kept change folds into it through @p fold instead, keeping the change's
 *  stamp: a write or a removal below makes it a write, and a change below composes with it, its
 *  bytes landing in @p arena. A version a range tombstone of @p runs hides from every reader turns
 *  into a removal before that, so a change above it folds onto nothing, and such removals drop once
 *  the chains are folded, as the tombstone stays with the merged runs. The versions view the runs'
 *  keys and bytes, so the runs outlive them until a merged run or a flushed one copies them.
 */
template <typename run_type_, utilities::fold_concept fold_type_, typename versions_type_>
status_t kept_versions(std::span<run_type_ const* const> runs, compaction_stamps_t const& survival,
                       fold_type_ const& fold, versions_type_& versions,
                       utilities::arena_allocator<byte_t>& arena) noexcept {
    using version_t = stamped_delta<typename run_type_::stored_key_t>;
    std::size_t count = 0;
    std::size_t tombstones = 0;
    for (run_type_ const* run : runs) {
        count += run->size();
        tombstones += run->range_tombstones().size();
    }
    using positions_allocator_t =
        typename std::allocator_traits<typename versions_type_::allocator_type>::template rebind_alloc<std::size_t>;
    using tournament_allocator_t =
        typename std::allocator_traits<typename versions_type_::allocator_type>::template rebind_alloc<std::uint32_t>;
    st::vector<std::size_t, positions_allocator_t> positions {positions_allocator_t {versions.get_allocator()}};
    utilities::loser_tree<tournament_allocator_t> tournament {tournament_allocator_t {versions.get_allocator()}};
    // Every run sits in key order with a key's versions newest first, so a tournament of the runs'
    // heads merges them, an exhausted run's head preceding none.
    auto const precedes = [&](std::uint32_t first, std::uint32_t second) noexcept {
        run_type_ const& first_run = *runs[first];
        run_type_ const& second_run = *runs[second];
        if (positions[first] == first_run.size()) return false;
        if (positions[second] == second_run.size()) return true;
        if (first_run.stored_key(positions[first]) < second_run.stored_key(positions[second])) return true;
        if (second_run.stored_key(positions[second]) < first_run.stored_key(positions[first])) return false;
        return second_run.stamp(positions[second]) < first_run.stamp(positions[first]);
    };
    if (versions.reserve(count) != st::status_t::success_k ||
        positions.resize(runs.size()) != st::status_t::success_k ||
        tournament.reset(static_cast<std::uint32_t>(runs.size()), precedes) != st::status_t::success_k)
        return {status_code_t::out_of_memory_k, "A merge could not hold its versions"};
    for (std::size_t taken = 0; taken != count; ++taken) {
        std::uint32_t const head = tournament.least();
        run_type_ const& run = *runs[head];
        std::size_t const position = positions[head]++;
        [[maybe_unused]] st::status_t const placed = versions.push_back(
            st::assume_reserved, version_t {run.stored_key(position), run.stamp(position), run.delta(position)});
        tournament.replay(precedes);
    }
    auto const hidden = [&](version_t const& version) noexcept {
        return tombstones != 0 && hidden_by_range(runs, version.stored_key, version.stamp, survival);
    };
    for (version_t& version : versions)
        if (hidden(version)) version.delta = delta_view_t {delta_kind_t::remove_k, {}};
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
    std::size_t uncovered = 0;
    for (std::size_t index = 0; index != kept; ++index)
        if (versions[index].delta.kind != delta_kind_t::remove_k || !hidden(versions[index]))
            versions[uncovered++] = versions[index];
    while (versions.size() != uncovered) versions.pop_back();
    return {};
}

} // namespace unum::ustore::blobs
