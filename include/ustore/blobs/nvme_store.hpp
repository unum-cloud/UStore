/**
 *  @file include/ustore/blobs/nvme_store.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The NVMe store of one collection: immutable memory runs and a value cache in front of an LSM tree of sorted
 *      runs, the one view every read runs against, and the ordered walk over any value layout.
 *
 *  Three places can answer a key, and they are consulted newest first: the memory runs hold what has
 *  not been flushed, the value cache holds what a point read already paid for, and the tree holds the
 *  rest. Only the third moves bytes, and it moves them through the level set one view names, so a
 *  batched select reaches the queue pair's depth rather than the size of whatever batch it was handed.
 *  Nothing here allocates an I/O buffer. A caller's span is never registered memory, so the bytes are
 *  copied out of the channel's own blocks either way, and that one copy is the whole of the read path.
 *
 *  A value cache hit is copied out of a slot the cache refills in place, between two loads of the
 *  slot's sequence, so a read that finds a key there costs one hash probe and one copy, and a
 *  displaced value is never freed apart. The bound is a count rather than a byte total, derived
 *  once from the budget and the width the collection's values take.
 *
 *  A read copies no run and moves no byte to find its place: it announces itself on its own slot of
 *  the ledger's epochs, loads the published stamp and then the view, and walks the immutable runs
 *  the view names. A publication replaces the view beside rather than in place, so the runs a
 *  reader walks stay readable until it leaves. A snapshot is a pinned stamp, which every merge
 *  keeps the versions of.
 *
 *  A run answers a key slot by sorted position, so an ordered walk is a merge over one cursor per
 *  run, memory runs included, and nothing is materialized. Where several runs carry the same key
 *  the newest memory run wins, then the shallowest level, and inside a level that may overlap the
 *  youngest run. There is no prefetcher, and adding one would be a mistake rather than an
 *  optimization: a scan that wants bytes hands the next keys to @c nvme_store::select, which
 *  already refills the ring as completions retire, while a second pipeline reading ahead on the
 *  same channel would poll the first one's completions and drop them.
 */

#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <algorithm> // `std::max`, `std::min`, `std::upper_bound`
#include <array>     // `std::array`
#include <atomic>    // `std::memory_order_acquire`
#include <bit>       // `std::bit_floor`
#include <limits>    // `std::numeric_limits`
#include <memory>    // `std::unique_ptr`
#include <new>       // `std::nothrow`
#include <span>      // `std::span`
#include <utility>   // `std::move`, `std::swap`

#include <smashtable/basic_hash_table.hpp> // `st::assume_reserved`
#include <smashtable/basic_vector.hpp>     // `st::vector`
#include <smashtable/shared.hpp>           // `st::status_t`

#include "ustore/blobs/nvme_memory_run.hpp"     // `keyed_delta`, `memory_run`, `sealed_deltas`
#include "ustore/blobs/nvme_key_slot.hpp"       // `limit`, `stored_value_size_t`, `value_layout_t`
#include "ustore/blobs/nvme_value_cache.hpp"    // `value_cache`
#include "ustore/blobs/nvme_published_runs.hpp" // `published_runs`, `commit_ledger`, `stamp_pin`
#include "ustore/blobs/nvme_tree.hpp"           // `lsm_tree`, `lsm_tree_position_t`, `collection_config_t`
#include "ustore/utilities/algorithm.hpp"       // `sort_by_stored_key`, `heap_sort`, `heap_sift_up`
#include "ustore/utilities/folds.hpp"           // `fold_concept`
#include "ustore/utilities/nvme_channels.hpp"   // `io_channel_t`
#include "ustore/utilities/nvme_ring.hpp"       // `portable_alignment_k`
#include "ustore/utilities/ram_buffers.hpp" // `local_arena_t`, `arena_allocator`, `resource_constraint`, `constrained_buffer`
#include "ustore/utilities/types.hpp"       // `status_t`, `status_code_t`, `expected`, `byte_t`, `uuid_generator`

namespace unum::ustore::blobs {

using utilities::constrained_allocator;
using utilities::constrained_buffer;
using utilities::outside_executor_k;
using utilities::resource_constraint;

#pragma region Store

#pragma region Vocabulary

/** One commit's deltas for one collection, sealed off every shared path by its committer, and the run the leader
 *  stamps them into, both charged to a budget whose word @p atomic_reference_ owns; both outlive the request carrying
 *  them only until the commit lands or is refused. */
template <typename stored_key_type_, template <typename> typename atomic_reference_,
          typename row_kit_type_ = st::native_row_kit_t>
struct sealed_commit {

    using sealed_deltas_t =
        sealed_deltas<stored_key_type_, constrained_allocator<byte_t, atomic_reference_>, row_kit_type_>;

    /** The deltas, sorted, which the leader moves into a run of the store's pool once it draws the
     *  stamp. */
    sealed_deltas_t deltas;

    /** Whether the commit carries no delta and no range tombstone for this store, adding no run. */
    [[nodiscard]] bool empty() const noexcept { return deltas.size() == 0 && deltas.range_tombstones().empty(); }

    /** Takes over @p buffer when the deltas were sealed lending it, which the leader does once it
     *  draws the stamp, so the landed run owns the bytes it views and nothing past it allocates. */
    void adopt(typename sealed_deltas_t::buffer_t& buffer) noexcept { deltas.adopt(buffer); }
};

/** One key a scan hands out: what it carries at the scan's stamp and how wide its value is. */
template <typename stored_key_type_>
struct scan_entry {

    /** The key. */
    stored_key_type_ stored_key {};

    /** Whether the key carries a value or a removal. */
    lookup_result_t state = lookup_result_t::missing_k;

    /** Bytes the value takes, zero for a removal and for an empty value. */
    stored_value_size_t size = 0;
};

/** The order a scan hands its keys out in. */
enum class scan_direction_t : std::uint8_t {

    /** Smallest key first, from the start key up. */
    ascending_k,

    /** Largest key first, from the start key down. */
    descending_k,
};

/** Keys one page of a scan holds, as many as one page of memory does, which bounds how long a commit waits. */
template <typename stored_key_type_>
constexpr std::size_t scan_page_keys() noexcept {
    return utilities::arena_page_bytes_k / sizeof(scan_entry<stored_key_type_>);
}

/** The runs an unordered scan of one store walks one at a time: every level's runs in order, then the memory runs
 *  oldest first, so the run a later commit adds lands past every run counted. */
struct counted_runs_t {

    /** Runs counted. */
    std::uint64_t count = 0;

    /** The arrangement the count holds for, which a flush, a compaction or a merge of memory runs moves past. */
    std::uint64_t layout_generation = 0;
};

/** One change a feed of a collection hands out, which also marks where the feed stands. */
template <typename stored_key_type_>
struct stored_change {

    /** The stamp of the commit that made the change. */
    transaction_stamp_t stamp = 0;

    /** The key changed. */
    stored_key_type_ stored_key {};

    /** What the change did. */
    delta_kind_t kind = delta_kind_t::write_k;
};

/** Whether a feed hands out @p first before @p second: by stamp, then by key, as a commit changes
 *  each key once. */
template <typename stored_key_type_>
constexpr bool change_precedes(stored_change<stored_key_type_> const& first,
                               stored_change<stored_key_type_> const& second) noexcept {
    if (first.stamp != second.stamp) return first.stamp < second.stamp;
    return first.stored_key < second.stored_key;
}

template <typename key_slot_type_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, utilities::fold_concept fold_type_, typename device_fold_type_,
          typename row_kit_type_ = st::native_row_kit_t>
class nvme_store_cursor;

#pragma endregion Vocabulary

#pragma region Store

/**
 *  @brief The store of one collection: immutable memory runs and a value cache in front of an LSM tree of sorted runs,
 *      behind one published view.
 *
 *  A reader announces itself in its ledger's epochs, loads the published stamp and then the view, and walks immutable
 *  runs with no lock. A committer seals its deltas into one memory run on its own thread, and the ledger's leader
 *  publishes a view naming it ahead of the others. A merge folds F runs of one tier into one run of the next tier,
 *  and a flush writes the memory runs of one view as one run of level zero, each with no lock held, then takes the
 *  leader word only to publish a view without exactly those runs. Whoever replaces a view frees the view and the runs
 *  it no longer names before returning, once every reader that entered before has left. Merges, flushes, compactions
 *  and truncations of one store are serialized by its owner. The volume, the budget and every channel are borrowed
 *  and must outlive the store, and a channel belongs to the thread holding it.
 *
 *  The fold decides what a change delta does: a read collects a key's versions newest first down to the first that
 *  closes the chain and applies them, and every merge composes the changes no reader tells apart. A store of blobs
 *  folds with @c last_write_fold_t, writes no change, and reads and merges exactly as if it had no fold.
 *
 *  @tparam atomic_reference_ The reference the published view, the cache, the budget and the ledger own their words
 *      through.
 *  @tparam waiter_type_ What a core waiting on a read, a barrier or the ledger does between attempts, before it parks.
 *  @tparam fold_type_ What a change delta does, which reads and merges fold with.
 *  @tparam device_fold_type_ What composes the changes of a merge on a device, or @c no_device_fold_t where nothing
 *      does, which keeps every merge holding a change on the host.
 *  @tparam row_kit_type_ The SmashTable kit the memory runs' trees search with.
 */
template <typename key_slot_type_, typename volume_type_, template <typename> typename atomic_reference_,
          typename waiter_type_, utilities::fold_concept fold_type_, typename device_fold_type_,
          typename row_kit_type_ = st::native_row_kit_t>
class nvme_store {
  public:

#pragma region Types

    /** The key slot every run of this store holds. */
    using key_slot_t = key_slot_type_;

    /** The volume every run of this store lives on. */
    using volume_t = volume_type_;

    /** What a change delta of this store does, which reads and merges fold with. */
    using fold_t = fold_type_;

    /** What composes the changes of a merge on a device, or @c no_device_fold_t where nothing does. */
    using device_fold_t = device_fold_type_;

    /** The SmashTable kit the memory runs' trees search with. */
    using row_kit_t = row_kit_type_;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The values of versions point reads paid I/O for, which this collection keeps. */
    using value_cache_t = value_cache<stored_key_t, atomic_reference_>;

    /** The stamps this collection's writes carry and its readers pin. */
    using ledger_t = commit_ledger<atomic_reference_, waiter_type_>;

    /** One stamp a reader or a view of this collection holds against the ledger. */
    using pin_t = stamp_pin<atomic_reference_, waiter_type_>;

    /** The budget the memory runs and the cache are charged to. */
    using constraint_t = resource_constraint<atomic_reference_>;

    /** Where the sections of one flushed run sit inside its blob. */
    using sorted_run_layout_t = sorted_run_layout<key_slot_t>;

    /** One delta a commit hands over: the key, its kind, and its bytes. */
    using keyed_delta_t = keyed_delta<stored_key_t>;

    /** One commit's deltas, sorted and copied, before they are stamped. */
    using sealed_deltas_t = sealed_deltas<stored_key_t, constrained_allocator<byte_t, atomic_reference_>, row_kit_t>;

    /** The keys one range removal hides. */
    using range_t = range_tombstone<stored_key_t>;

    /** One commit's deltas, stamped, which nothing changes once a view names them. */
    using memory_run_t = memory_run<stored_key_t, constrained_allocator<byte_t, atomic_reference_>, row_kit_t>;

    /** The tree, memory runs and view this store publishes, and every merge, flush and compaction of them. */
    using published_runs_t =
        published_runs<key_slot_t, memory_run_t, volume_t, atomic_reference_, waiter_type_, row_kit_t>;

    /** The tree the flushed writes of this collection live in. */
    using tree_t = typename published_runs_t::tree_t;

    /** What one slice of a merge of that tree writes into, lent by the database. */
    using merge_buffers_t = typename tree_t::merge_buffers_t;

    /** One immutable run of that tree, which an ingest writes before it publishes. */
    using sorted_run_t = typename tree_t::sorted_run_t;

    /** One level set of that tree, which a view names. */
    using level_set_t = typename tree_t::level_set_t;

    /** The database's pool the runs of that tree live in. */
    using run_pool_t = typename tree_t::run_pool_t;

    /** Everything a reader of this collection needs, which nothing changes once it is published. */
    using view_t = typename published_runs_t::view_t;

    /** One commit's deltas for this store between their sealing and their landing. */
    using sealed_commit_t = sealed_commit<stored_key_t, atomic_reference_, row_kit_t>;

    /** One version a merge or a flush carries over from a memory run. */
    using stamped_delta_t = stamped_delta<stored_key_t>;

    /** One change a feed of this collection hands out. */
    using stored_change_t = stored_change<stored_key_t>;

    /** One reader inside the ledger's epochs, announced until it dies, which keeps every view
     *  loaded meanwhile. */
    using reader_t = typename published_runs_t::reader_t;

    /** The view a read walks and the stamp it answers at, which every read of a call may share. */
    using reading_t = typename published_runs_t::reading_t;

    /** How the values of this collection are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

    /** Whether the values sit in the key slots themselves, so reading one needs no I/O. */
    static constexpr bool holds_values_inline_k = is_inline(value_layout_k);

#pragma endregion Types

#pragma region Construction

    nvme_store() noexcept = default;
    nvme_store(nvme_store const&) = delete;
    nvme_store& operator=(nvme_store const&) = delete;
    nvme_store(nvme_store&&) noexcept = default;
    nvme_store& operator=(nvme_store&&) noexcept = default;

    /**
     *  @brief An empty store over @p volume, whose memory is charged to @p constraint.
     *  @param[in,out] volume The volume every run lands on, borrowed and outliving the store.
     *  @param[in] config The bounds of the memory runs, the cache bound and the shape of the tree below.
     *  @param[in,out] constraint The budget the memory runs and the cache are charged to.
     *  @param[in,out] ledger The stamps the store's writes carry and its readers pin, outliving the store.
     *  @param[in,out] runs The database's pool the tree's runs live in, outliving the store.
     */
    static expected<nvme_store> mount(volume_t& volume, collection_config_t const& config, constraint_t& constraint,
                                      ledger_t& ledger, run_pool_t& runs) noexcept {
        sorted_run_config_t const& values = config.lsm_tree.lsm_level_0.sorted_run;
        nvme_store mounted;
        mounted.value_size_ = is_fixed(value_layout_k) ? values.value_size : 0;
        mounted.value_max_size_ = is_inline(value_layout_k)
                                      ? static_cast<stored_value_size_t>(value_layout_k)
                                      : (is_fixed(value_layout_k) ? values.value_size : values.value_max_size);
        if (mounted.value_max_size_ == 0)
            return status_t {status_code_t::invalid_argument_k, "A collection admits values of some width"};
        mounted.cache_ = value_cache_t::of(holds_values_inline_k ? 0 : config.cache_limit, mounted.value_max_size_,
                                           constraint);
        if (status_t const mounted_runs = mounted.runs_.mount(volume, config, constraint, ledger, runs);
            mounted_runs.failed())
            return mounted_runs;
        return mounted;
    }

    /** Returns every run to the volume and forgets every memory run, which destroys the data and which only a
     *  collection no reader reaches and no manifest names may. */
    void destroy() noexcept { runs_.destroy(); }

#pragma endregion Construction

#pragma region Shape

    /** The bounds this collection was mounted with. */
    [[nodiscard]] collection_config_t const& config() const noexcept { return runs_.config(); }

    /** The tree the flushed writes live in, which a database compacts and names in its schema under its writer. */
    [[nodiscard]] tree_t& tree() noexcept { return runs_.tree(); }

    /** The tree the flushed writes live in. */
    [[nodiscard]] tree_t const& tree() const noexcept { return runs_.tree(); }

    /** Bytes the largest value this collection admits takes. */
    [[nodiscard]] stored_value_size_t value_max_size() const noexcept { return value_max_size_; }

    /** Deltas the memory runs hold, which no flush has written yet. */
    [[nodiscard]] std::size_t memory_deltas_count() const noexcept { return runs_.memory_deltas_count(); }

    /** How many memory runs the published view names, and how many commits they hold, which merges by tier keep
     *  within (F − 1)(T + 1) runs for T = ⌈log_F commits⌉ once every commit returned, F being the enlarge factor. */
    [[nodiscard]] memory_shape_t memory_shape() const noexcept { return runs_.memory_shape(); }

    /** Whether no memory run and no level holds a key. */
    [[nodiscard]] bool empty() const noexcept { return population_count() == 0; }

    /** Deltas the memory runs hold plus key slots every level holds, one key counted once per run holding it. */
    [[nodiscard]] std::uint64_t population_count() const noexcept { return runs_.population_count(); }

    /** Bytes every run of every level takes on its volume. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept { return runs_.size_bytes(); }

#pragma endregion Shape

#pragma region Lookups

    /** Announces executor thread @p thread_index in the ledger's epochs until the answer dies,
     *  which keeps every reading taken meanwhile walkable. */
    [[nodiscard]] reader_t enter(std::size_t thread_index = outside_executor_k) const noexcept {
        return runs_.enter(thread_index);
    }

    /** The view a read at @p stamp walks and the stamp it answers at, for a caller inside the
     *  ledger's epochs, which every read of one call may walk with no pin, as the epochs keep the
     *  view and its runs alive. */
    [[nodiscard]] reading_t reading(transaction_stamp_t stamp) const noexcept { return runs_.reading(stamp); }

    /**
     *  @brief Whether anything carries @p stored_key, and how many bytes its value takes.
     *  @param[in] stored_key The key to look for.
     *  @param[out] size Bytes the value takes, which is zero for anything but @c written_k.
     *  @param[in] stamp The stamp a reader is pinned at, or @c head_stamp_k for the published one.
     *  @param[in] thread_index The executor thread reading, which announces on its own epoch slot.
     *  @return What the key carries, or @c incomplete_k for a fold's chain reaching a level whose
     *      values only a @c select reads.
     */
    [[nodiscard]] lookup_result_t find(stored_key_t stored_key, stored_value_size_t& size,
                                       transaction_stamp_t stamp = head_stamp_k,
                                       std::size_t thread_index = outside_executor_k) const noexcept {
        reader_t const inside = runs_.enter(thread_index);
        return find(runs_.reading(stamp), stored_key, size);
    }

    /** As @c find above, through @p read, which a caller inside the ledger's epochs took for every
     *  read of a call. */
    [[nodiscard]] lookup_result_t find(reading_t const& read, stored_key_t stored_key,
                                       stored_value_size_t& size) const noexcept {
        size = 0;
        delta_view_t found;
        transaction_stamp_t version = unwritten_stamp_k;
        if (read.view->memory_runs.find(stored_key, read.stamp, found, version)) {
            if (hidden_in_memory_(read, stored_key, version)) return lookup_result_t::removed_k;
            if (found.kind == delta_kind_t::change_k) return find_folded_(read, stored_key, size);
            if (found.kind == delta_kind_t::remove_k) return lookup_result_t::removed_k;
            size = static_cast<stored_value_size_t>(found.bytes.size());
            return lookup_result_t::written_k;
        }
        lookup_result_t const in_levels = find_in_levels_(read, stored_key, size, version);
        if (in_levels == lookup_result_t::changed_k) return find_folded_(read, stored_key, size);
        return selected_(in_levels);
    }

    /**
     *  @brief Copies the value of @p stored_key into @p into, which needs only the value's own bytes.
     *  @param[in,out] channel The channel the read is submitted and completed on, held by the calling thread.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     *  @param[in] stamp The stamp a reader is pinned at, or @c head_stamp_k for the published one.
     *  @param[in] thread_index The executor thread reading, which announces on its own epoch slot.
     *  @return @c not_found_k for a miss or a removal, @c input_output_k for a run shorter than its index claims.
     *
     *  Not @c const: a read that pays I/O files the version it read in the value cache.
     */
    status_t select(io_channel_t& channel, stored_key_t stored_key, std::span<byte_t> into, stored_value_size_t& size,
                    transaction_stamp_t stamp = head_stamp_k, std::size_t thread_index = outside_executor_k) noexcept {
        reader_t const inside = runs_.enter(thread_index);
        return select(channel, runs_.reading(stamp), stored_key, into, size);
    }

    /** As @c select above, through @p read, which a caller inside the ledger's epochs took for
     *  every read of a call. */
    status_t select(io_channel_t& channel, reading_t const& read, stored_key_t stored_key, std::span<byte_t> into,
                    stored_value_size_t& size) noexcept {
        size = 0;
        delta_view_t found;
        transaction_stamp_t found_version = unwritten_stamp_k;
        if (read.view->memory_runs.find(stored_key, read.stamp, found, found_version)) {
            if (hidden_in_memory_(read, stored_key, found_version))
                return {status_code_t::not_found_k, "A commit removed a range holding that key"};
            if (found.kind == delta_kind_t::change_k) return select_folded_(channel, read, stored_key, into, size);
            if (found.kind == delta_kind_t::remove_k) return {status_code_t::not_found_k, "A commit removed that key"};
            if (into.size() < found.bytes.size())
                return {status_code_t::invalid_argument_k, "A buffer shorter than the value"};
            if (!found.bytes.empty()) std::memcpy(into.data(), found.bytes.data(), found.bytes.size());
            size = static_cast<stored_value_size_t>(found.bytes.size());
            return {};
        }

        level_set_t const& levels = *read.view->levels;
        stored_value_size_t wanted = 0;
        transaction_stamp_t version = unwritten_stamp_k;
        lookup_result_t const in_levels = find_in_levels_(read, stored_key, wanted, version);
        if (in_levels == lookup_result_t::changed_k) return select_folded_(channel, read, stored_key, into, size);
        if (in_levels != lookup_result_t::written_k) return {status_code_t::not_found_k, "No level carries that key"};
        if (into.size() < wanted) return {status_code_t::invalid_argument_k, "A buffer shorter than the value"};

        if constexpr (!holds_values_inline_k)
            if (cache_.copy(stored_key, version, into, size)) return {};

        std::uint64_t delivered = 0;
        auto sink = [&](std::size_t, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            if (!bytes.empty()) std::memcpy(into.data() + offset, bytes.data(), bytes.size());
            delivered += bytes.size();
        };
        std::array<stored_key_t, 1> const keys {stored_key};
        std::array<lookup_result_t, 1> answers {lookup_result_t::missing_k};
        if (status_t const read_status = runs_.tree().read_values(channel, levels, std::span<stored_key_t const> {keys},
                                                                  std::span<lookup_result_t> {answers}, sink,
                                                                  read.stamp);
            read_status.failed())
            return read_status;
        // One key has no rest of a batch to keep, so a block short of its last one fails the whole call.
        if (delivered != wanted) return {status_code_t::io_k, "A run is shorter than its index claims"};
        size = wanted;

        if constexpr (!holds_values_inline_k) cache_.add(stored_key, version, into.first(size));
        return {};
    }

    /**
     *  @brief Reads the values of @p stored_keys, refilling the channel as completions retire.
     *  @param[in,out] channel The channel the reads are submitted and completed on, held by the calling thread.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from a removal.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     *  @param[in] stamp The stamp a reader is pinned at, or @c head_stamp_k for the published one.
     *  @param[in] thread_index The executor thread reading, which announces on its own epoch slot.
     *  @return Success, or the failure that stopped the batch rather than one of its keys.
     *
     *  @note A value larger than one block arrives as several calls, each carrying the bytes of the
     *      value before it, and a key whose value is present and empty is reported once with no bytes.
     *  @note The bytes a batched read moves are never cached, since caching one would mean staging the
     *      whole value in a buffer this path exists to do without.
     */
    template <typename sink_type_>
    status_t select(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<lookup_result_t> results, sink_type_&& sink, transaction_stamp_t stamp = head_stamp_k,
                    std::size_t thread_index = outside_executor_k) const noexcept {
        reader_t const inside = runs_.enter(thread_index);
        utilities::local_arena_t scratch;
        return select(channel, runs_.reading(stamp), scratch.allocator().arena, stored_keys, results, sink);
    }

    /** As the batched @c select above, through @p read, which a caller inside the ledger's epochs
     *  took for every read of a call, carving what it tracks from @p scratch, which the caller
     *  rewinds once per call. */
    template <typename sink_type_>
    status_t select(io_channel_t& channel, reading_t const& read, ustore_arena_t scratch,
                    std::span<stored_key_t const> stored_keys, std::span<lookup_result_t> results,
                    sink_type_&& sink) const noexcept {
        if (results.size() != stored_keys.size())
            return {status_code_t::invalid_argument_k, "A batched select answers one result per key"};
        level_set_t const& levels = *read.view->levels;

        // The tree takes its keys contiguous, so the request each deferred key came from travels
        // beside it, and a key whose newest version is a change folds once the reads retired.
        auto [deferred_keys, keys_status] = utilities::arena_allocate<stored_key_t>(scratch, stored_keys.size());
        auto [deferred_requests, requests_status] = utilities::arena_allocate<std::size_t>(scratch, stored_keys.size());
        auto [folded_requests, folded_status] = utilities::arena_allocate<std::size_t>(scratch, stored_keys.size());
        if (keys_status.failed() || requests_status.failed() || folded_status.failed())
            return {status_code_t::out_of_memory_k, "A collection could not track the keys of a batch"};
        std::size_t deferred = 0;
        std::size_t folded = 0;

        // A cached value is copied out before the sink sees it, into one room grown to the widest.
        std::span<byte_t> cached_value;
        bool const tombstoned = tombstoned_(read);
        for (std::size_t request = 0; request != stored_keys.size(); ++request) {
            delta_view_t found;
            transaction_stamp_t found_version = unwritten_stamp_k;
            if (read.view->memory_runs.find(stored_keys[request], read.stamp, found, found_version)) {
                if (hidden_in_memory_(read, stored_keys[request], found_version))
                    results[request] = lookup_result_t::removed_k;
                else if (found.kind == delta_kind_t::change_k) {
                    results[request] = lookup_result_t::missing_k;
                    folded_requests[folded++] = request;
                }
                else if (found.kind == delta_kind_t::remove_k) results[request] = lookup_result_t::removed_k;
                else {
                    results[request] = lookup_result_t::written_k;
                    sink(request, std::uint64_t {0}, found.bytes);
                }
                continue;
            }
            // The tree reads past range tombstones, so a key one hides never reaches it.
            if (tombstoned) {
                stored_value_size_t wanted = 0;
                transaction_stamp_t version = unwritten_stamp_k;
                if (find_in_levels_(read, stored_keys[request], wanted, version) == lookup_result_t::removed_k) {
                    results[request] = lookup_result_t::removed_k;
                    continue;
                }
            }
            if constexpr (!holds_values_inline_k)
                if (cache_.capacity() != 0) {
                    stored_value_size_t wanted = 0;
                    stored_value_size_t copied = 0;
                    transaction_stamp_t version = unwritten_stamp_k;
                    bool cached = find_in_levels_(read, stored_keys[request], wanted, version) ==
                                  lookup_result_t::written_k;
                    if (cached && cached_value.size() < wanted) {
                        auto [grown, grown_status] = utilities::arena_allocate<byte_t>(scratch, wanted);
                        cached = grown_status.succeeded();
                        if (cached) cached_value = grown;
                    }
                    if (cached && cache_.copy(stored_keys[request], version, cached_value, copied)) {
                        results[request] = lookup_result_t::written_k;
                        sink(request, std::uint64_t {0}, std::span<byte_t const> {cached_value}.first(copied));
                        continue;
                    }
                }
            results[request] = lookup_result_t::missing_k;
            deferred_keys[deferred] = stored_keys[request];
            deferred_requests[deferred++] = request;
        }
        if (deferred == 0)
            return select_folded_(channel, read, scratch, stored_keys,
                                  std::span<std::size_t const> {folded_requests.first(folded)}, results, sink);

        // The tree answers in its own vocabulary and the sink is told nothing about sizes, so the bytes every key
        // owed are counted here and compared once the read has run.
        auto [answers, answers_status] = utilities::arena_allocate<lookup_result_t>(scratch, deferred);
        auto [delivered, delivered_status] = utilities::arena_allocate<std::uint64_t>(scratch, deferred);
        if (answers_status.failed() || delivered_status.failed())
            return {status_code_t::out_of_memory_k, "A collection could not track the keys of a batch"};
        for (std::size_t index = 0; index != deferred; ++index) {
            answers[index] = lookup_result_t::missing_k;
            delivered[index] = 0;
        }

        auto relay = [&](std::size_t index, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            delivered[index] += bytes.size();
            sink(deferred_requests[index], offset, bytes);
        };
        status_t const read_status = runs_.tree().read_values(
            channel, levels, std::span<stored_key_t const> {deferred_keys.first(deferred)}, answers, relay, read.stamp);

        std::size_t truncated = 0;
        for (std::size_t index = 0; index != deferred; ++index) {
            lookup_result_t& result = results[deferred_requests[index]];
            if (answers[index] == lookup_result_t::missing_k) continue;
            if (answers[index] == lookup_result_t::removed_k) {
                result = lookup_result_t::removed_k;
                continue;
            }
            if (answers[index] == lookup_result_t::changed_k) {
                folded_requests[folded++] = deferred_requests[index];
                continue;
            }
            stored_value_size_t wanted = 0;
            transaction_stamp_t version = unwritten_stamp_k;
            [[maybe_unused]] lookup_result_t const carried = find_in_levels_(read, deferred_keys[index], wanted,
                                                                             version);
            if (delivered[index] == wanted) result = lookup_result_t::written_k;
            else {
                result = lookup_result_t::incomplete_k;
                ++truncated;
            }
        }

        // A block short of what it asked for fails its own key; a cancelled or refused one fails the batch.
        if (read_status.failed() && (truncated == 0 || !(read_status == status_code_t::io_k))) return read_status;
        return select_folded_(channel, read, scratch, stored_keys,
                              std::span<std::size_t const> {folded_requests.first(folded)}, results, sink);
    }

    /** The runs a reader at @p stamp walks, counted for an unordered scan that walks them one at a time. */
    [[nodiscard]] counted_runs_t count_runs(transaction_stamp_t stamp = head_stamp_k,
                                            std::size_t thread_index = outside_executor_k) const noexcept {
        reader_t const inside = runs_.enter(thread_index);
        return count_runs(runs_.reading(stamp));
    }

    /** The runs a reader through @p read walks, which a caller inside the ledger's epochs took for
     *  every read of a call, so no flush rearranges them before @c scan_run walks them. */
    [[nodiscard]] counted_runs_t count_runs(reading_t const& read) const noexcept {
        return counted_runs_t {level_runs_(*read.view->levels) + read.view->memory_runs.runs().size(),
                               read.view->layout_generation};
    }

    /**
     *  @brief Hands @p visitor the newest version at or below @p stamp of every key from @p from
     *      up that the run at @p run_index of @p counted carries, in that run's order, with no
     *      other run consulted.
     *  @param[in] counted The runs as @c count_runs counted them, which the index names one of.
     *  @param[in] run_index Which of the counted runs to walk.
     *  @param[in] from The first key to hand out.
     *  @param[in] visitor Called as @c visitor(stored_key, state, size) inside the epochs, answering whether the walk
     *      carries on, so it must not block; a change answers @c changed_k with its own bytes.
     *  @param[in] stamp The stamp a reader is pinned at, which every run of one scan should share.
     *  @param[in] thread_index The executor thread reading, which announces on its own epoch slot.
     *  @retval contended_k A flush, a compaction or a merge of memory runs rearranged the runs since they were counted.
     *  @retval invalid_argument_k The index names no run counted.
     */
    template <typename visitor_type_>
    status_t scan_run(counted_runs_t const& counted, std::uint64_t run_index, stored_key_t from,
                      visitor_type_&& visitor, transaction_stamp_t stamp = head_stamp_k,
                      std::size_t thread_index = outside_executor_k) const noexcept {
        reader_t const inside = runs_.enter(thread_index);
        return scan_run(runs_.reading(stamp), counted, run_index, from, visitor);
    }

    /** As @c scan_run above, through @p read, which a caller inside the ledger's epochs took for
     *  every read of a call. */
    template <typename visitor_type_>
    status_t scan_run(reading_t const& read, counted_runs_t const& counted, std::uint64_t run_index, stored_key_t from,
                      visitor_type_&& visitor) const noexcept {
        if (run_index >= counted.count) return {status_code_t::invalid_argument_k, "No run counted at that index"};
        if (read.view->layout_generation != counted.layout_generation)
            return {status_code_t::contended_k, "The runs were rearranged since they were counted"};
        level_set_t const& levels = *read.view->levels;
        std::uint64_t index = run_index;
        for (std::size_t number = 0; number != levels.size(); ++number) {
            auto const& level = levels.level(number);
            if (index >= level.size()) {
                index -= level.size();
                continue;
            }
            auto const& run = level.run(static_cast<std::size_t>(index));
            walk_versions_(
                run.lower_bound(from), run.population_count(), read.stamp,
                [&](std::uint64_t position) noexcept { return run.key_slot(position).stored_key; },
                [&](std::uint64_t position) noexcept { return run.stamp(position); },
                [&](std::uint64_t position) noexcept {
                    stored_key_t const stored_key = run.key_slot(position).stored_key;
                    lsm_tree_position_t const at {static_cast<std::uint32_t>(number),
                                                  lsm_level_position_t {static_cast<std::uint32_t>(index), position}};
                    if (hidden_in_levels_(read, stored_key, at))
                        return visitor(stored_key, lookup_result_t::removed_k, stored_value_size_t {0});
                    return visitor(stored_key, run_result_(run.kind(position)),
                                   static_cast<stored_value_size_t>(run.value_window(position).bytes));
                });
            return {};
        }
        // Commits only put newer runs ahead in the view, so the oldest-first index counts from its end.
        std::span<memory_run_t const* const> const memory = read.view->memory_runs.runs();
        if (index >= memory.size()) return {status_code_t::invalid_argument_k, "No run counted at that index"};
        memory_run_t const& run = *memory[memory.size() - 1 - static_cast<std::size_t>(index)];
        walk_versions_(
            run.lower_bound(from), run.size(), read.stamp,
            [&](std::uint64_t position) noexcept { return run.stored_key(static_cast<std::size_t>(position)); },
            [&](std::uint64_t position) noexcept { return run.stamp(static_cast<std::size_t>(position)); },
            [&](std::uint64_t position) noexcept {
                std::size_t const sorted = static_cast<std::size_t>(position);
                if (hidden_in_memory_(read, run.stored_key(sorted), run.stamp(sorted)))
                    return visitor(run.stored_key(sorted), lookup_result_t::removed_k, stored_value_size_t {0});
                delta_view_t const delta = run.delta(sorted);
                return visitor(run.stored_key(sorted), run_result_(delta.kind),
                               static_cast<stored_value_size_t>(delta.bytes.size()));
            });
        return {};
    }

    /** Pins @p pin at @p stamp for a feed of this collection's changes, so no merge folds a version
     *  above it, refusing with @c write_conflict_k a stamp a merge already folded past. */
    status_t pin_feed(pin_t& pin, transaction_stamp_t stamp) noexcept { return runs_.pin_feed(pin, stamp); }

    /**
     *  @brief Hands @p visitor up to @p wanted changes committed after @p after, oldest first and a
     *      commit's by key, as the published view holds them: every version a merge kept, and a
     *      range removal as the removal of every key it hid a value of.
     *  @param[in,out] channel This thread's channel for reading values outside their key slots.
     *  @param[in] after The last change handed out, or one at its pinned stamp past all keys.
     *  @param[in] wanted Changes to hand out at most.
     *  @param[in] visitor Called as <tt>visitor(change, value_bytes, read)</tt> inside the epochs, where
     *      @c read(into) copies the change's @c value_bytes bytes into @p into, answering a status;
     *      a failure ends the walk and is answered.
     *  @param[in] thread_index The executor thread reading, which announces on its own epoch slot.
     *
     *  Nothing orders a run by stamp, so a pull reads the stamps of every run holding one at or
     *  past the stamp of @p after, keeping the @p wanted earliest changes. A range removal stamped
     *  @c t expands into the keys a walk at <tt>t - 1</tt> finds a value for, but those its own commit
     *  wrote again; every merge keeps the version a reader at <tt>t - 1</tt> sees, as the feed pins a
     *  stamp below @c t.
     */
    template <typename visitor_type_>
    status_t changes_after(io_channel_t& channel, stored_change_t const& after, std::size_t wanted,
                           visitor_type_&& visitor, std::size_t thread_index = outside_executor_k) const noexcept {
        reader_t const inside = runs_.enter(thread_index);
        reading_t const read = runs_.reading(head_stamp_k);
        if (wanted == 0 || read.stamp < after.stamp) return {};
        level_set_t const& levels = *read.view->levels;
        std::span<memory_run_t const* const> const memory = read.view->memory_runs.runs();

        // A range removal hides at most one version of every key any run holds.
        std::uint64_t versions = 0;
        std::uint64_t held_versions = 0;
        std::uint64_t tombstones = 0;
        for (memory_run_t const* run : memory) {
            held_versions += run->size();
            if (run->newest_stamp() < after.stamp) continue;
            versions += run->size();
            tombstones += run->range_tombstones().size();
        }
        for (std::size_t number = 0; number != levels.size(); ++number)
            for (std::size_t index = 0; index != levels.level(number).size(); ++index) {
                auto const& run = levels.level(number).run(index);
                held_versions += run.population_count();
                if (run.newest_stamp() < after.stamp) continue;
                versions += run.population_count();
                tombstones += run.range_tombstones().size();
            }
        std::uint64_t const bound = std::min<std::uint64_t>(wanted, versions + (tombstones != 0 ? held_versions : 0));
        if (bound == 0) return {};
        located_changes_t selected {constrained_allocator<located_change_t, atomic_reference_> {runs_.constraint()}};
        stamped_ranges_t ranges {constrained_allocator<stamped_range_t, atomic_reference_> {runs_.constraint()}};
        if (selected.reserve(bound) != st::status_t::success_k || ranges.reserve(tombstones) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to select the changes of a feed"};

        // A heap of the earliest changes offered so far, the latest of them on top.
        auto const earlier = [](located_change_t const& first, located_change_t const& second) noexcept {
            return change_precedes(first.change, second.change);
        };
        auto offer = [&](located_change_t const& located) noexcept {
            if (located.change.stamp > read.stamp || !change_precedes(after, located.change)) return;
            if (selected.size() < bound) {
                [[maybe_unused]] st::status_t const placed = selected.push_back(st::assume_reserved,
                                                                                located_change_t {located});
                utilities::heap_sift_up(std::span<located_change_t> {selected}, earlier);
                return;
            }
            if (!earlier(located, selected.front())) return;
            selected.front() = located;
            utilities::heap_sift_down(std::span<located_change_t> {selected}, 0, earlier);
        };
        auto gather_ranges = [&]<typename tombstones_type_>(tombstones_type_ const& held) noexcept {
            for (std::size_t index = 0; index != held.size(); ++index) {
                if (held.stamp(index) < after.stamp) continue;
                [[maybe_unused]] st::status_t const placed = ranges.push_back(
                    st::assume_reserved, stamped_range_t {held.stamp(index), range_t {held.range(index)}});
            }
        };

        for (memory_run_t const* run : memory) {
            if (run->newest_stamp() < after.stamp) continue;
            for (std::size_t position = 0; position != run->size(); ++position)
                if (run->stamp(position) >= after.stamp)
                    offer(
                        located_change_t {{run->stamp(position), run->stored_key(position), run->delta(position).kind},
                                          run,
                                          lsm_tree_position_t {0, lsm_level_position_t {0, position}}});
            gather_ranges(run->range_tombstones());
        }
        for (std::size_t number = 0; number != levels.size(); ++number)
            for (std::size_t index = 0; index != levels.level(number).size(); ++index) {
                auto const& run = levels.level(number).run(index);
                if (run.newest_stamp() < after.stamp) continue;
                for (std::uint64_t position = 0; position != run.population_count(); ++position)
                    if (run.stamp(position) >= after.stamp)
                        offer(located_change_t {
                            {run.stamp(position), run.key_slot(position).stored_key, run.kind(position)},
                            nullptr,
                            lsm_tree_position_t {static_cast<std::uint32_t>(number),
                                                 lsm_level_position_t {static_cast<std::uint32_t>(index), position}}});
                gather_ranges(run.range_tombstones());
            }

        // A commit's meeting ranges, like clipped merge pieces, walk as one, so keys return once.
        utilities::heap_sort(std::span<stamped_range_t> {ranges},
                             [](stamped_range_t const& first, stamped_range_t const& second) noexcept {
                                 if (first.stamp != second.stamp) return first.stamp < second.stamp;
                                 return first.range.start < second.range.start;
                             });
        std::size_t joined = 0;
        for (std::size_t index = 0; index != ranges.size(); ++index) {
            stamped_range_t& last = ranges[joined == 0 ? 0 : joined - 1];
            if (joined != 0 && last.stamp == ranges[index].stamp && !(last.range.end < ranges[index].range.start)) {
                if (last.range.end < ranges[index].range.end) last.range.end = ranges[index].range.end;
                continue;
            }
            ranges[joined++] = ranges[index];
        }
        // Each range's walk carves its run cursors here.
        utilities::local_arena_t walks;
        for (std::size_t index = 0; index != joined; ++index) {
            stamped_range_t const& removed = ranges[index];
            if (removed.stamp > read.stamp) continue;
            stored_key_t const from = removed.stamp == after.stamp && removed.range.start < after.stored_key
                                          ? after.stored_key
                                          : removed.range.start;
            auto [walk, walk_status] = cursor_t::over(*this, walks.allocator().arena, from, removed.stamp - 1,
                                                      thread_index);
            if (walk_status.failed()) return walk_status;
            for (std::uint64_t expanded = 0;
                 expanded != bound && !walk.reached_end() && walk.stored_key() < removed.range.end;) {
                stored_change_t const change {removed.stamp, walk.stored_key(), delta_kind_t::remove_k};
                bool const held = walk.state() == lookup_result_t::written_k ||
                                  walk.state() == lookup_result_t::incomplete_k;
                if (held && change_precedes(after, change) && !rewritten_at_(read, change)) {
                    offer(located_change_t {change});
                    ++expanded;
                }
                if (status_t const advanced = walk.advance(); advanced.failed()) return advanced;
            }
        }

        utilities::heap_sort(std::span<located_change_t> {selected}, earlier);
        for (located_change_t const& located : selected) {
            auto read_value = [&](std::span<byte_t> into) noexcept {
                return read_change_(channel, levels, located, into);
            };
            if (status_t const visited = visitor(located.change, change_bytes_(levels, located), read_value);
                visited.failed())
                return visited;
        }
        return {};
    }

#pragma endregion Lookups

#pragma region Writes

    /** Whether a value of @p size bytes is one this collection's layout can hold, which @c seal checks. */
    [[nodiscard]] bool admits(std::size_t size) const noexcept { return admits_(size); }

    /**
     *  @brief Sorts @p deltas into one commit's sealed deltas for this store, on the committer's
     *      thread and off every shared path; no deltas seal into no run.
     *  @param[in] deltas The commit's deltas for this store, one per key, which this sorts in place: writes of an
     *      admitted width, removals, and a fold's changes, which only a run file's length bounds unless they sit
     *      inline in a key slot.
     *  @param[in] ranges The ranges the commit removes, each hiding every older version.
     *  @param[in] lent The bytes a transaction of this store alone wrote, which every delta views
     *      and the commit's run takes over once stamped, or none, which copies the bytes once.
     */
    expected<sealed_commit_t> seal(std::span<keyed_delta_t> deltas, std::span<range_t const> ranges = {},
                                   std::span<byte_t const> lent = {}) const noexcept {
        for (keyed_delta_t const& each : deltas)
            if (!admits_(each.delta))
                return status_t {status_code_t::malformed_k, "This collection admits no delta of that size"};
        sealed_commit_t sealed;
        if (deltas.empty() && ranges.empty()) return sealed;
        auto [sealed_deltas, seal_status] = sealed_deltas_t::seal(
            deltas, ranges, constrained_allocator<byte_t, atomic_reference_> {runs_.constraint()}, lent);
        if (seal_status.failed()) return seal_status;
        sealed.deltas = std::move(sealed_deltas);
        return sealed;
    }

    /** How full the memory runs of the published view would be with @p sealed beside them. */
    [[nodiscard]] memory_pressure_t memory_pressure(sealed_commit_t const& sealed) const noexcept {
        return runs_.memory_pressure(sealed.deltas.size(),
                                     sealed.deltas.values_bytes() + sealed.deltas.range_tombstones().bytes());
    }

    /** Claims a memory-run record and the room of the view naming its run, or answers
     *  @c capacity_exhausted_k while every record is taken, which the claimer meets by flushing
     *  or by waiting on @c vacated. */
    status_t try_claim(memory_run_claim_t& claimed) noexcept { return runs_.try_claim(claimed); }

    /** Takes back what @p claimed holds, which this store claimed and nobody prepared. */
    void give_back(memory_run_claim_t& claimed) noexcept { runs_.give_back(claimed); }

    /** The count of memory-run records vacated so far, which a claimer finding every one
     *  taken waits on. */
    [[nodiscard]] std::uint32_t const& vacated() const noexcept { return runs_.vacated(); }

    /**
     *  @brief Lays out the view naming @p sealed's run ahead of the current ones, out of the room
     *      @p claimed holds, so @c publish cannot fail.
     *  @note Called by the thread holding the ledger's leader word, which then publishes or forgets
     *      it before it lets the word go.
     */
    void prepare(sealed_commit_t& sealed, memory_run_claim_t&& claimed) noexcept {
        if (sealed.empty()) runs_.forget_prepared();
        else runs_.prepare(std::move(claimed));
    }

    /** Stamps @p sealed's run with @p stamp, publishes the view @c prepare laid out, and takes the run over, which the
     *  leader does before it publishes @p stamp itself, so a reader at the stamp finds the run. */
    void publish(sealed_commit_t& sealed, transaction_stamp_t stamp) noexcept {
        if (sealed.empty()) return;
        expected<memory_run_t> stamped = memory_run_t::stamped(
            std::move(sealed.deltas), stamp, constrained_allocator<byte_t, atomic_reference_> {runs_.constraint()});
        assert(stamped.has_value() && "A drawn stamp packs into no words");
        runs_.publish(stamped ? std::move(*stamped) : memory_run_t {}, stamp);
    }

    /** Drops the view @c prepare laid out, which the leader does for a commit it then refuses. */
    void forget_prepared() noexcept { runs_.forget_prepared(); }

    /** Lands @p sealed as a commit of its own through the ledger's sequencer, in the
     *  record @p claimed holds, and returns once it is published; a commit sealing no run
     *  draws no stamp. */
    status_t commit(sealed_commit_t& sealed, memory_run_claim_t&& claimed) noexcept {
        if (sealed.empty()) return {};
        alone_t alone {this, &sealed, &claimed};
        commit_request_t request;
        request.context = &alone;
        request.prepare = &prepare_alone_;
        request.publish = &publish_alone_;
        return runs_.ledger().submit(request).status();
    }

    /**
     *  @brief Whether a commit newer than @p from wrote @p stored_key, which only the newest write
     *      of a key decides, or removed a range holding it.
     *
     *  Answered from the memory runs first, then from the runs newest first, skipping every run whose newest stamp
     *  is not past @p from, since no version it holds can be.
     */
    [[nodiscard]] bool written_since(stored_key_t stored_key, transaction_stamp_t from) const noexcept {
        reader_t const inside = runs_.enter();
        view_t const& view = *runs_.view();
        for (memory_run_t const* run : view.memory_runs.runs()) {
            if (run->newest_stamp() <= from) return false;
            if (run->range_tombstones().overlaps_since(stored_key, stored_key, from)) return true;
            std::size_t const position = run->lower_bound(stored_key);
            if (position != run->size() && run->stored_key(position) == stored_key) return run->stamp(position) > from;
        }
        level_set_t const& taken = *view.levels;
        if (levels_removed_since_(taken, stored_key, stored_key, from)) return true;
        for (std::size_t number = 0; number != taken.size(); ++number) {
            auto const& level = taken.level(number);
            if (level.order() == lsm_level_order_t::sorted_k) {
                lsm_level_position_t position;
                if (level.find(stored_key, position) == lookup_result_t::missing_k) continue;
                auto const& run = level.run(position.run_index);
                return run.stamp(position.key_slot_position) > from;
            }
            for (std::size_t index = level.size(); index-- != 0;) {
                auto const& run = level.run(index);
                if (run.newest_stamp() <= from) continue;
                if (stored_key < run.lower_stored_key() || run.upper_stored_key() < stored_key) continue;
                std::uint64_t const position = run.position_of(stored_key);
                if (position != run.population_count()) return run.stamp(position) > from;
            }
        }
        return false;
    }

    /**
     *  @brief Whether a commit newer than @p from removed @p stored_key or wrote it whole, which
     *      an existence watch asks: change deltas since leave the key standing, so the versions
     *      past them decide.
     *
     *  Versions are walked newest first, memory runs before levels, until one is a removal or a
     *  write, which answers yes, or one is stamped at or below @p from, which answers no.
     */
    [[nodiscard]] bool replaced_since(stored_key_t stored_key, transaction_stamp_t from) const noexcept {
        reader_t const inside = runs_.enter();
        view_t const& view = *runs_.view();
        for (memory_run_t const* run : view.memory_runs.runs()) {
            if (run->newest_stamp() <= from) return false;
            if (run->range_tombstones().overlaps_since(stored_key, stored_key, from)) return true;
            for (std::size_t position = run->lower_bound(stored_key);
                 position != run->size() && run->stored_key(position) == stored_key; ++position) {
                if (run->stamp(position) <= from) return false;
                if (run->delta(position).kind != delta_kind_t::change_k) return true;
            }
        }
        level_set_t const& taken = *view.levels;
        if (levels_removed_since_(taken, stored_key, stored_key, from)) return true;
        for (std::size_t number = 0; number != taken.size(); ++number) {
            auto const& level = taken.level(number);
            for (std::size_t index = level.size(); index-- != 0;) {
                auto const& run = level.run(index);
                if (run.newest_stamp() <= from) continue;
                if (stored_key < run.lower_stored_key() || run.upper_stored_key() < stored_key) continue;
                for (std::uint64_t position = run.lower_bound(stored_key);
                     position != run.population_count() && run.key_slot(position).stored_key == stored_key;
                     ++position) {
                    if (run.stamp(position) <= from) return false;
                    if (run.kind(position) != delta_kind_t::change_k) return true;
                }
            }
        }
        return false;
    }

    /**
     *  @brief Whether a commit newer than @p from wrote any key from @p lower to @p upper inclusive, which is what a
     *      walk over the range watches, so a key inserted or removed where it found none conflicts too.
     *
     *  Every version counts, not only the newest, since a key written twice since @p from was written since. A run
     *  is skipped by its newest stamp.
     */
    [[nodiscard]] bool range_written_since(stored_key_t lower, stored_key_t upper,
                                           transaction_stamp_t from) const noexcept {
        if (upper < lower) return false;
        reader_t const inside = runs_.enter();
        view_t const& view = *runs_.view();
        if (view.memory_runs.range_written_since(lower, upper, from)) return true;
        level_set_t const& taken = *view.levels;
        if (levels_removed_since_(taken, lower, upper, from)) return true;
        for (std::size_t number = 0; number != taken.size(); ++number) {
            auto const& level = taken.level(number);
            for (std::size_t index = 0; index != level.size(); ++index) {
                auto const& run = level.run(index);
                if (run.newest_stamp() <= from) continue;
                if (upper < run.lower_stored_key() || run.upper_stored_key() < lower) continue;
                for (std::uint64_t position = run.lower_bound(lower);
                     position != run.population_count() && !(upper < run.key_slot(position).stored_key); ++position)
                    if (run.stamp(position) > from) return true;
            }
        }
        return false;
    }

    /** Opens the runs @p runs names as this collection's tree, with the stamps ingests assigned,
     *  which a mount does before anything reads. */
    status_t restore(io_channel_t& channel, std::span<lsm_tree_run_record_t const> runs) noexcept {
        return runs_.restore(channel, runs);
    }

    /** Writes @p stored_value under @p stored_key as a commit of its own, flushing first when the memory runs are past
     *  their bounds, which the caller serializes with every other flush. */
    status_t insert(io_channel_t& channel, stored_key_t stored_key, std::span<byte_t const> stored_value) noexcept {
        keyed_delta_t delta {stored_key, delta_view_t {delta_kind_t::write_k, stored_value}};
        return commit_alone_(channel, seal(std::span<keyed_delta_t> {&delta, 1}));
    }

    /** Removes @p stored_key as a commit of its own, flushing first as @c insert does. */
    status_t remove(io_channel_t& channel, stored_key_t stored_key) noexcept {
        keyed_delta_t delta {stored_key, delta_view_t {delta_kind_t::remove_k, {}}};
        return commit_alone_(channel, seal(std::span<keyed_delta_t> {&delta, 1}));
    }

    /** Removes every key from @p start up to @p end, exclusive, as one range tombstone committed on
     *  its own, flushing first as @c insert does. */
    status_t remove_range(io_channel_t& channel, stored_key_t start, stored_key_t end) noexcept {
        range_t const range {start, end};
        return commit_alone_(channel, seal(std::span<keyed_delta_t> {}, std::span<range_t const> {&range, 1}));
    }

    /**
     *  @brief Writes one value per key as one commit of its own, flushing first as @c insert does.
     *  @param[in,out] channel The channel a flush writes on, held by the writer.
     *  @param[in] stored_keys The keys to write, in any order and with repeats taking the last value.
     *  @param[in] stored_values The values back to back, in the order their keys name them.
     *  @param[in] sizes Bytes of every value, one per key, which must add up to @p stored_values.
     */
    status_t insert(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<byte_t const> stored_values, std::span<stored_value_size_t const> sizes) noexcept {
        return commit_alone_(channel, seal_writes(stored_keys, stored_values, sizes));
    }

    /** Removes every key of @p stored_keys as one commit of its own, flushing first as @c insert does. */
    status_t remove(io_channel_t& channel, std::span<stored_key_t const> stored_keys) noexcept {
        return commit_alone_(channel, seal_removals(stored_keys));
    }

    /** Seals one value per key of @p stored_keys, a repeated key taking its last value, as @c insert takes them. */
    expected<sealed_commit_t> seal_writes(std::span<stored_key_t const> stored_keys,
                                          std::span<byte_t const> stored_values,
                                          std::span<stored_value_size_t const> sizes) const noexcept {
        if (status_t const checked = check_batch(stored_keys, stored_values, sizes); checked.failed()) return checked;
        auto [deltas, deltas_status] = deltas_of_(stored_keys.size());
        if (deltas_status.failed()) return deltas_status;
        std::size_t offset = 0;
        for (std::size_t index = 0; index != stored_keys.size(); ++index) {
            deltas[index] = keyed_delta_t {
                stored_keys[index], delta_view_t {delta_kind_t::write_k, stored_values.subspan(offset, sizes[index])}};
            offset += sizes[index];
        }
        return seal(last_of_each_key_(deltas));
    }

    /** Seals a removal of every key of @p stored_keys, as @c remove takes them. */
    expected<sealed_commit_t> seal_removals(std::span<stored_key_t const> stored_keys) const noexcept {
        auto [deltas, deltas_status] = deltas_of_(stored_keys.size());
        if (deltas_status.failed()) return deltas_status;
        for (std::size_t index = 0; index != stored_keys.size(); ++index)
            deltas[index] = keyed_delta_t {stored_keys[index], delta_view_t {delta_kind_t::remove_k, {}}};
        return seal(last_of_each_key_(deltas));
    }

    /** Whether @p sizes names one admitted width per key of @p stored_keys and adds up to @p stored_values. */
    status_t check_batch(std::span<stored_key_t const> stored_keys, std::span<byte_t const> stored_values,
                         std::span<stored_value_size_t const> sizes) const noexcept {
        if (sizes.size() != stored_keys.size())
            return {status_code_t::invalid_argument_k, "A batched insert names the size of every value"};
        std::uint64_t total = 0;
        for (std::size_t index = 0; index != sizes.size(); ++index) {
            if (!admits_(sizes[index]))
                return {status_code_t::malformed_k, "This collection admits no value of that size"};
            total += sizes[index];
        }
        if (total != stored_values.size())
            return {status_code_t::invalid_argument_k, "The named sizes do not add up to the values"};
        return {};
    }

#pragma endregion Writes

#pragma region Maintenance

    /**
     *  @brief Merges F memory runs of one tier into one run of the next tier, F being the level enlarge factor, until
     *      no tier holds F runs, the lowest tier and its oldest runs first.
     *
     *  Every merge reads immutable runs with no lock held, keeps the versions a pinned reader needs and every version
     *  above the stamp published when it began, and takes the leader word only to publish a view naming the merged run
     *  in place of exactly those runs; letting the word go, it waits for the readers that could hold them, then frees
     *  them before the next merge.
     *  @note The owner serializes it with flushes, compactions and truncations of this collection, and the calling
     *      thread is inside no epoch.
     */
    status_t merge_memory_runs() noexcept { return runs_.merge_memory_runs(fold_); }

    /** Whether the memory runs hold the bounds they were mounted with, so a flush is due. */
    [[nodiscard]] bool flush_due() const noexcept { return runs_.flush_due(); }

    /** Whether the memory runs are due for a merge, holding F runs of tier zero, or for a flush; the commit that
     *  made them so runs either before it returns. */
    [[nodiscard]] bool maintenance_due() const noexcept { return runs_.maintenance_due(); }

    /**
     *  @brief Writes the memory runs of the published view as one run of level zero with no lock held, then publishes
     *      a view naming that run instead of exactly those runs, keeping every memory run committed meanwhile.
     *  @note The owner serializes flushes, compactions and truncations of one collection, and the calling thread is
     *      inside no epoch, since the publication waits for every reader that could hold a run it frees.
     */
    status_t flush(io_channel_t& channel) noexcept {
        return runs_.flush([&](tree_t& tree, std::span<memory_run_t const* const> flushed,
                               compaction_stamps_t const& survival,
                               lsm_tree_publication_hook_t const& published) noexcept -> status_t {
            drained_t drained {runs_.constraint()};
            stamped_deltas_t versions {constrained_allocator<stamped_delta_t, atomic_reference_> {runs_.constraint()}};
            // The values a flush folds sit here until the run is written, beside the runs' own.
            utilities::local_arena_t folded;
            utilities::arena_allocator<byte_t> folded_bytes = folded.allocator();
            if (status_t const kept = kept_versions(flushed, survival, fold_, versions, folded_bytes); kept.failed())
                return kept;
            if (status_t const laid = lay_out_versions_(std::span<stamped_delta_t const> {versions}, drained);
                laid.failed())
                return laid;
            auto [tombstones, tombstones_status] = gathered_range_tombstones(flushed, st::default_allocator<byte_t> {});
            if (tombstones_status.failed()) return tombstones_status;
            laid_out_values_t const values = laid_out_values_(std::span<stamped_delta_t const> {versions}, drained, 0);
            return tree.flush(channel, std::span<key_slot_t const> {drained.key_slots},
                              std::span<std::uint64_t const> {drained.stamps},
                              std::span<std::uint8_t const> {drained.kinds}, survival.floor, values,
                              std::span<stored_value_offset_t const> {drained.value_offsets}, value_max_size_,
                              published, {}, tombstones);
        });
    }

    /** What an ingest keeps between its writes: the rows laid out for the run it fills, and the runs it wrote. */
    struct ingest_runs_t;

    /** What an ingest into this collection starts from, charging the store's budget. */
    [[nodiscard]] ingest_runs_t begin_ingest() noexcept { return ingest_runs_t {drained_t {runs_.constraint()}}; }

    /**
     *  @brief Lays @p rows out behind the rows @p ingested laid out before, at stamp zero, writing a run each time one
     *      fills one slot.
     *  @param[in,out] channel The channel the runs are written on, held by the calling thread.
     *  @param[in] rows Rows in ascending order of their keys, readable until the call returns.
     *  @return Success, or the status that stopped it with @p ingested as it was: @c invalid_argument_k for keys that
     *      do not strictly ascend past every key before them, @c malformed_k for a row this collection admits no value
     *      of.
     *  @note Allocates on the volume, so the caller holds what serializes the volume's placements.
     */
    status_t write_ingest(io_channel_t& channel, std::span<keyed_delta_t const> rows,
                          ingest_runs_t& ingested) noexcept {
        if (rows.empty()) return {};
        drained_t& drained = ingested.drained;
        bool const continues = !ingested.runs.empty() || !drained.key_slots.empty();
        std::uint64_t bytes = 0;
        for (std::size_t index = 0; index != rows.size(); ++index) {
            stored_key_t const& previous = index ? rows[index - 1].stored_key : ingested.last;
            if ((index || continues) && !(previous < rows[index].stored_key))
                return {status_code_t::invalid_argument_k, "An ingest takes its keys strictly ascending"};
            if (!admits_(rows[index].delta))
                return {status_code_t::malformed_k, "This collection admits no value of that size"};
            bytes += is_fixed(value_layout_k) ? value_size_ : rows[index].delta.bytes.size();
        }
        // The rows laid out before take a run of their own when these overflow it, so no run below holds one of them
        if (!drained.key_slots.empty() &&
            ingested_run_overflows_(drained.key_slots.size() + rows.size(), drained.stored_values.size() + bytes))
            if (status_t const written = write_ingested_run_(channel, ingested); written.failed()) return written;
        std::size_t const runs_before = ingested.runs.size();
        std::size_t const slots_before = drained.key_slots.size();
        std::size_t const values_before = drained.stored_values.size();
        if (status_t const laid = lay_out_ingested_(channel, rows, ingested); laid.failed()) {
            while (ingested.runs.size() != runs_before) {
                runs_.tree().volume().deallocate(ingested.runs.back().extent());
                ingested.runs.pop_back();
            }
            drained.resize(slots_before, values_before);
            return laid;
        }
        ingested.last = rows.back().stored_key;
        return {};
    }

    /**
     *  @brief Writes the rows @p ingested laid out last as its final run, and publishes every run as one commit placed
     *      above every level holding their keys, flushing the memory runs first and retrying while commits refill them.
     *  @return The stamp every ingested version carries, or the status that stopped it with every run dropped.
     *  @note The owner serializes it with flushes, compactions and truncations, and the calling thread is inside no
     *      epoch.
     */
    expected<transaction_stamp_t> publish_ingest(io_channel_t& channel, ingest_runs_t& ingested) noexcept {
        if (!ingested.drained.key_slots.empty())
            if (status_t const written = write_ingested_run_(channel, ingested); written.failed()) {
                drop_ingest(ingested);
                return written;
            }
        return runs_.publish_ingest(ingested.runs, [&]() noexcept { return flush(channel); });
    }

    /** Gives back every run @p ingested wrote and drops every row it laid out, which an ingest that
     *  never published leaves. */
    void drop_ingest(ingest_runs_t& ingested) noexcept {
        for (sorted_run_t const& run : ingested.runs)
            if (run.attached()) runs_.tree().volume().deallocate(run.extent());
        ingested.runs.clear();
        ingested.drained.clear();
    }

    /** Whether any level of the tree is over the bytes its number admits. */
    [[nodiscard]] bool needs_compaction() const noexcept { return runs_.needs_compaction(); }

    /**
     *  @brief Merges the shallowest level that is over its bounds into the level below it.
     *  @param[in,out] reading_channel The channel the merge gathers source values on, held by the calling thread.
     *  @param[in,out] writing_channel The channel the merge writes its new runs on, which is not @p reading_channel.
     *  @param[in,out] buffers What each slice writes into, at least one per slice of @p fan_out.
     *  @param[in] compactor The compactor the database picked at open: the calling thread, or its warm device
     *      compactor.
     *  @param[in] published What to call once the merged set is live and before a consumed run is dropped.
     *  @param[in] fan_out The threads a merge on the host fans its slices out over, or one slice on
     *      the calling thread.
     */
    expected<compaction_outcome_t> compact(
        io_channel_t& reading_channel, io_channel_t& writing_channel, std::span<merge_buffers_t> buffers,
        compactor_ref_t const& compactor, lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {},
        compaction_fan_out_t const& fan_out = compaction_fan_out_t {}) noexcept {
        return runs_.compact(reading_channel, writing_channel, buffers, compactor, fold_, device_fold_, published,
                             fan_out);
    }

    /** Drops every version a mount found stamped above @p ceiling, which only a collection holding no memory run
     *  takes, merging into the lent @p buffers. */
    status_t truncate_above(io_channel_t& reading_channel, io_channel_t& writing_channel,
                            std::span<merge_buffers_t> buffers, transaction_stamp_t ceiling,
                            lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {}) noexcept {
        return runs_.truncate_above(reading_channel, writing_channel, buffers, ceiling, fold_, published);
    }

#pragma endregion Maintenance

  private:

    friend class nvme_store_cursor<key_slot_t, volume_t, atomic_reference_, waiter_type_, fold_t, device_fold_t,
                                   row_kit_t>;

#pragma region Private Types

    /** One commit of this store alone, as the sequencer hands it to its two steps. */
    struct alone_t {

        /** The store committed to. */
        nvme_store* owner = nullptr;

        /** What its committer sealed. */
        sealed_commit_t* sealed = nullptr;

        /** The record its committer claimed for the run. */
        memory_run_claim_t* claimed = nullptr;
    };

    /** A column one flush writes, charging the store's budget. */
    template <typename element_type_>
    using drained_column = st::vector<element_type_, constrained_allocator<element_type_, atomic_reference_>>;

    /** The columns one flush or one ingested run writes, every one charging the store's budget. */
    struct drained_t {
        drained_t() noexcept = default;
        explicit drained_t(constraint_t& constraint) noexcept
            : key_slots {constrained_allocator<key_slot_t, atomic_reference_> {constraint}},
              stamps {constrained_allocator<std::uint64_t, atomic_reference_> {constraint}},
              kinds {constrained_allocator<std::uint8_t, atomic_reference_> {constraint}},
              stored_values {constrained_allocator<byte_t, atomic_reference_> {constraint}},
              value_offsets {constrained_allocator<stored_value_offset_t, atomic_reference_> {constraint}} {}

        /** The key slots, in ascending order of their keys. */
        drained_column<key_slot_t> key_slots;

        /** The stamps of the key slots, in the same order. */
        drained_column<std::uint64_t> stamps;

        /** The @c delta_kind_t of the key slots, in the same order. */
        drained_column<std::uint8_t> kinds;

        /** The values, back to back in the order of the key slots. */
        drained_column<byte_t> stored_values;

        /** Where each value starts and one past the last, for a variable width alone. */
        drained_column<stored_value_offset_t> value_offsets;

        /** Empties every column for the next run, keeping what they hold room for. */
        void clear() noexcept {
            key_slots.clear(), stamps.clear(), kinds.clear(), stored_values.clear(), value_offsets.clear();
        }

        /** Trims the columns back to @p slots key slots and @p values bytes of values, as they stood before a layout,
         *  the value offsets to one past the slots, or none for none. */
        void resize(std::size_t slots, std::size_t values) noexcept {
            [[maybe_unused]] st::status_t trimmed = key_slots.resize(slots);
            trimmed = stamps.resize(slots);
            trimmed = kinds.resize(slots);
            trimmed = stored_values.resize(values);
            trimmed = value_offsets.resize(value_offsets.empty() || slots == 0 ? 0 : slots + 1);
        }
    };

    /** The versions one merge or one flush carries over, before and after they are sorted into a run's order. */
    using stamped_deltas_t = st::vector<stamped_delta_t, constrained_allocator<stamped_delta_t, atomic_reference_>>;

    /** The values of laid-out versions, read where the versions view them, which a flush gathers
     *  straight into its write blocks and an ingest into the run it fills: back to back, a stride
     *  zero-padded at a fixed width, and none for a layout holding them inline. */
    struct laid_out_values_t {

        /** The versions, in the order their key slots were laid out. */
        std::span<stamped_delta_t const> versions;

        /** Where each value starts and one past the last, for a variable width alone. */
        std::span<stored_value_offset_t const> value_offsets;

        /** Bytes every value takes at a fixed width, zero otherwise. */
        std::uint64_t stride = 0;

        /** Bytes the values take together. */
        [[nodiscard]] std::uint64_t size() const noexcept {
            if constexpr (holds_values_inline_k) return 0;
            else if constexpr (is_fixed(value_layout_k)) return versions.size() * stride;
            else return value_offsets.back() - value_offsets.front();
        }

        /** Fills @p into with the bytes of the values from byte @p from on. */
        void gather(std::uint64_t from, std::span<byte_t> into) const noexcept {
            if (into.empty()) return;
            std::size_t index = 0;
            std::uint64_t begin = 0;
            if constexpr (is_fixed(value_layout_k)) {
                index = static_cast<std::size_t>(from / stride);
                begin = index * stride;
            }
            else {
                stored_value_offset_t const wanted = static_cast<stored_value_offset_t>(value_offsets.front() + from);
                index = static_cast<std::size_t>(std::upper_bound(value_offsets.begin(), value_offsets.end(), wanted) -
                                                 value_offsets.begin() - 1);
                begin = value_offsets[index] - value_offsets.front();
            }
            for (; !into.empty(); ++index) {
                std::uint64_t const end = is_fixed(value_layout_k) ? begin + stride
                                                                   : value_offsets[index + 1] - value_offsets.front();
                std::span<byte_t const> const bytes = versions[index].delta.bytes;
                std::size_t const within = static_cast<std::size_t>(from - begin);
                std::size_t const taken = static_cast<std::size_t>(std::min<std::uint64_t>(into.size(), end - from));
                std::size_t const left = within < bytes.size() ? bytes.size() - within : 0;
                std::size_t const carried = std::min(taken, left);
                if (carried != 0) std::memcpy(into.data(), bytes.data() + within, carried);
                if (carried != taken) std::memset(into.data() + carried, 0, taken - carried);
                into = into.subspan(taken);
                from += taken;
                begin = end;
            }
        }
    };

    /** The deltas one commit of its own hands over. */
    using deltas_t = st::vector<keyed_delta_t, constrained_allocator<keyed_delta_t, atomic_reference_>>;

  public:
    struct ingest_runs_t {

        /** The key slots, stamps, kinds, values and value offsets laid out for the run being filled. */
        drained_t drained;

        /** The runs written so far, in ascending order of their keys. */
        st::vector<sorted_run_t> runs;

        /** The last key laid out, which the next row must ascend past. */
        stored_key_t last {};
    };

  private:

    /** One change a feed pull selected, and where the view it read holds it. */
    struct located_change_t {

        /** The change itself. */
        stored_change_t change;

        /** The memory run holding it, or null for a version of a level and for a range removal. */
        memory_run_t const* memory_run = nullptr;

        /** Its sorted position in @c memory_run, or its place among the levels. */
        lsm_tree_position_t position;
    };

    /** The changes one feed pull selects. */
    using located_changes_t = st::vector<located_change_t, constrained_allocator<located_change_t, atomic_reference_>>;

    /** One range tombstone a feed pull expands, and the stamp of the commit that wrote it. */
    struct stamped_range_t {

        /** The stamp of that commit. */
        transaction_stamp_t stamp = 0;

        /** The keys it hid. */
        range_t range;
    };

    /** The range tombstones one feed pull expands. */
    using stamped_ranges_t = st::vector<stamped_range_t, constrained_allocator<stamped_range_t, atomic_reference_>>;

    /** The walk a feed pull expands a range tombstone with. */
    using cursor_t =
        nvme_store_cursor<key_slot_t, volume_t, atomic_reference_, waiter_type_, fold_t, device_fold_t, row_kit_t>;

#pragma endregion Private Types

#pragma region Private Methods

    /** Bytes the change @p located carries, zero for a removal. */
    [[nodiscard]] static std::uint64_t change_bytes_(level_set_t const& levels,
                                                     located_change_t const& located) noexcept {
        if (utilities::removes(located.change.kind)) return 0;
        if (located.memory_run)
            return located.memory_run
                ->delta(static_cast<std::size_t>(located.position.level_position.key_slot_position))
                .bytes.size();
        return levels.level(located.position.level_number).value_window(located.position.level_position).bytes;
    }

    /** Whether the commit of @p change wrote a version of its key itself, which outlives that
     *  commit's range removal and comes back as its own change. */
    [[nodiscard]] bool rewritten_at_(reading_t const& read, stored_change_t const& change) const noexcept {
        delta_view_t found;
        transaction_stamp_t version = unwritten_stamp_k;
        if (read.view->memory_runs.find(change.stored_key, change.stamp, found, version))
            return version == change.stamp;
        level_set_t const& levels = *read.view->levels;
        lsm_tree_position_t position;
        if (runs_.tree().find(levels, change.stored_key, position, change.stamp) == lookup_result_t::missing_k)
            return false;
        lsm_level_position_t const& at = position.level_position;
        return levels.level(position.level_number).run(at.run_index).stamp(at.key_slot_position) == change.stamp;
    }

    /** Copies the bytes of the change @p located into @p into, reading a level's on @p channel
     *  where they sit apart from the key slot. */
    static status_t read_change_(io_channel_t& channel, level_set_t const& levels, located_change_t const& located,
                                 std::span<byte_t> into) noexcept {
        std::uint64_t const bytes = change_bytes_(levels, located);
        if (into.size() < bytes) return {status_code_t::invalid_argument_k, "A buffer shorter than the change"};
        if (bytes == 0) return {};
        lsm_level_position_t const& at = located.position.level_position;
        if (located.memory_run) {
            std::memcpy(into.data(),
                        located.memory_run->delta(static_cast<std::size_t>(at.key_slot_position)).bytes.data(), bytes);
            return {};
        }
        return levels.level(located.position.level_number)
            .run(at.run_index)
            .read_value(channel, at.key_slot_position, into);
    }

    /** Runs every level of @p levels holds together. */
    [[nodiscard]] static std::uint64_t level_runs_(level_set_t const& levels) noexcept {
        std::uint64_t runs = 0;
        for (std::size_t number = 0; number != levels.size(); ++number) runs += levels.level(number).size();
        return runs;
    }

    /** What a lookup answers for a run's delta of @p kind, taken alone. */
    [[nodiscard]] static lookup_result_t run_result_(delta_kind_t kind) noexcept {
        if (kind == delta_kind_t::remove_k) return lookup_result_t::removed_k;
        if (kind == delta_kind_t::change_k) return lookup_result_t::changed_k;
        return lookup_result_t::written_k;
    }

    /** Visits through @p visit the newest position at or below @p stamp of every key of one run's sorted positions
     *  @b [begin,end), whose versions of one key sit newest first, until @p visit answers false. */
    template <typename key_of_type_, typename stamp_of_type_, typename visit_type_>
    static void walk_versions_(std::uint64_t begin, std::uint64_t end, transaction_stamp_t stamp, key_of_type_&& key_of,
                               stamp_of_type_&& stamp_of, visit_type_&& visit) noexcept {
        std::uint64_t visited = end;
        for (std::uint64_t position = begin; position != end; ++position) {
            if (stamp_of(position) > stamp) continue;
            if (visited != end && key_of(visited) == key_of(position)) continue;
            visited = position;
            if (!visit(position)) return;
        }
    }

    /** Whether a written value of @p size bytes is one this collection's layout can hold. */
    [[nodiscard]] bool admits_(std::size_t size) const noexcept {
        if constexpr (is_fixed(value_layout_k)) return size == value_size_;
        else return size <= value_max_size_;
    }

    /** Whether this collection's layout can hold @p delta: a write of an admitted width, a removal, or a change that
     *  fits the key slot where values sit inline and the stride where they sit at a fixed width, the written-value
     *  bounds binding writes alone otherwise. */
    [[nodiscard]] bool admits_(delta_view_t const& delta) const noexcept {
        if (delta.kind == delta_kind_t::write_k) return admits_(delta.bytes.size());
        if constexpr (holds_values_inline_k) return delta.bytes.size() <= value_max_size_;
        else if constexpr (is_fixed(value_layout_k))
            return delta.kind == delta_kind_t::remove_k || delta.bytes.size() == value_size_;
        else return true;
    }

    /** Whether a range tombstone of a run of @p levels stamped after @p from hides a key from
     *  @p lower to @p upper inclusive. */
    [[nodiscard]] static bool levels_removed_since_(level_set_t const& levels, stored_key_t const& lower,
                                                    stored_key_t const& upper, transaction_stamp_t from) noexcept {
        for (std::size_t number = 0; number != levels.size(); ++number) {
            auto const& level = levels.level(number);
            if (level.range_tombstones_count() == 0) continue;
            for (std::size_t index = 0; index != level.size(); ++index)
                if (level.run(index).range_tombstones().overlaps_since(lower, upper, from)) return true;
        }
        return false;
    }

    /** Whether any memory run or level @p read walks carries a range tombstone. */
    [[nodiscard]] static bool tombstoned_(reading_t const& read) noexcept {
        for (memory_run_t const* run : read.view->memory_runs.runs())
            if (!run->range_tombstones().empty()) return true;
        level_set_t const& levels = *read.view->levels;
        for (std::size_t number = 0; number != levels.size(); ++number)
            if (levels.level(number).range_tombstones_count() != 0) return true;
        return false;
    }

    /** Whether a range tombstone @p read sees hides the version of @p stored_key a memory run holds
     *  stamped @p version, which only a newer memory run's tombstone may, as the memory runs order
     *  by stamp alone. */
    [[nodiscard]] static bool hidden_in_memory_(reading_t const& read, stored_key_t const& stored_key,
                                                transaction_stamp_t version) noexcept {
        return read.view->memory_runs.covering_stamp(stored_key, read.stamp) > version;
    }

    /**
     *  @brief Whether a range tombstone @p read sees hides @p stored_key's version at @p position.
     *
     *  A merge raises a version's stamp to its floor while a tombstone keeps its own, so positions
     *  decide across runs: a tombstone of a memory run, of a shallower level or of a younger run of
     *  an overlapping level hides the version, and one of the version's own run hides it when
     *  stamped above it. A sorted level's other runs hide none of its keys, as their bounds cover
     *  their tombstones and never overlap.
     */
    [[nodiscard]] static bool hidden_in_levels_(reading_t const& read, stored_key_t const& stored_key,
                                                lsm_tree_position_t const& position) noexcept {
        if (read.view->memory_runs.covering_stamp(stored_key, read.stamp) != unwritten_stamp_k) return true;
        level_set_t const& levels = *read.view->levels;
        std::size_t const own = position.level_position.run_index;
        for (std::size_t number = 0; number <= position.level_number; ++number) {
            auto const& level = levels.level(number);
            if (level.range_tombstones_count() == 0) continue;
            bool const shallower = number != position.level_number;
            for (std::size_t index = 0; index != level.size(); ++index) {
                bool const younger = level.order() == lsm_level_order_t::overlapping_k && index > own;
                if (!shallower && !younger && index != own) continue;
                auto const& run = level.run(index);
                transaction_stamp_t const covering = run.range_tombstones().covering_stamp(stored_key, read.stamp);
                if (covering == unwritten_stamp_k) continue;
                if (shallower || younger) return true;
                if (covering > run.stamp(position.level_position.key_slot_position)) return true;
            }
        }
        return false;
    }

    /** What the levels of @p read answer for @p stored_key at its stamp, the bytes its value takes,
     *  and the stamp of @p version; a change answers its own bytes, which the versions below it
     *  complete, and a version a range tombstone hides answers a removal. */
    [[nodiscard]] lookup_result_t find_in_levels_(reading_t const& read, stored_key_t stored_key,
                                                  stored_value_size_t& size,
                                                  transaction_stamp_t& version) const noexcept {
        size = 0;
        level_set_t const& levels = *read.view->levels;
        lsm_tree_position_t position;
        lookup_result_t const found = runs_.tree().find(levels, stored_key, position, read.stamp);
        if (found == lookup_result_t::missing_k || found == lookup_result_t::removed_k) return found;
        if (hidden_in_levels_(read, stored_key, position)) return lookup_result_t::removed_k;
        auto const& level = levels.level(position.level_number);
        size = static_cast<stored_value_size_t>(level.value_window(position.level_position).bytes);
        version = level.run(position.level_position.run_index).stamp(position.level_position.key_slot_position);
        return found;
    }

    /** What a lookup of the levels answering @p found tells a caller, a change aside. */
    [[nodiscard]] static lookup_result_t selected_(lookup_result_t found) noexcept {
        if (found == lookup_result_t::missing_k) return lookup_result_t::missing_k;
        if (found == lookup_result_t::removed_k) return lookup_result_t::removed_k;
        return lookup_result_t::written_k;
    }

    /**
     *  @brief Folds the versions of @p stored_key at or below @p read's stamp, collected newest first down to the first
     *      that closes the chain, into the value a reader at that stamp sees.
     *  @param[in] read The read whose stamp bounds the versions folded.
     *  @param[in] stored_key The key whose versions are folded.
     *  @param[in] bytes_of Called as @c bytes_of(run, position, bytes) for a version a level holds at a sorted
     *      position, viewing its bytes in the key slot or in @p arena, answering a status.
     *  @param[in,out] arena Where the bytes the fold reads and writes are allocated.
     *  @param[out] value The folded value, viewing a memory run, a key slot, or @p arena.
     *  @return What the key carries, or the status a read or the fold refused it with.
     */
    template <typename bytes_of_type_>
    expected<lookup_result_t> fold_versions_(reading_t const& read, stored_key_t stored_key, bytes_of_type_&& bytes_of,
                                             utilities::arena_allocator<byte_t>& arena,
                                             std::span<byte_t const>& value) const noexcept {
        value = {};
        st::vector<delta_view_t, utilities::arena_allocator<delta_view_t>> chain {
            utilities::arena_allocator<delta_view_t> {arena}};
        delta_view_t closing {delta_kind_t::remove_k, {}};
        bool closed = false;
        for (transaction_stamp_t at = read.stamp;;) {
            delta_view_t version;
            transaction_stamp_t stamp = unwritten_stamp_k;
            // A range tombstone hiding a version closes the chain as a removal for the reader.
            if (read.view->memory_runs.find(stored_key, at, version, stamp)) {
                if (hidden_in_memory_(read, stored_key, stamp))
                    version = delta_view_t {delta_kind_t::remove_range_k, {}};
            }
            else {
                lsm_tree_position_t position;
                lookup_result_t const found = runs_.tree().find(*read.view->levels, stored_key, position, at);
                if (found == lookup_result_t::missing_k) break;
                auto const& run =
                    read.view->levels->level(position.level_number).run(position.level_position.run_index);
                std::uint64_t const sorted = position.level_position.key_slot_position;
                stamp = run.stamp(sorted);
                version.kind = run.kind(sorted);
                if (hidden_in_levels_(read, stored_key, position)) version.kind = delta_kind_t::remove_range_k;
                else if (run.value_window(sorted).bytes != 0)
                    if (status_t const read_bytes = bytes_of(run, sorted, version.bytes); read_bytes.failed())
                        return read_bytes;
            }
            if (version.kind != delta_kind_t::change_k || fold_.closes(version)) {
                closing = version;
                closed = true;
                break;
            }
            if (chain.push_back(delta_view_t {version}) != st::status_t::success_k)
                return status_t {status_code_t::out_of_memory_k, "No room to collect a key's changes"};
            // Versions of one key carry distinct stamps, so the next one older sits at or below this stamp less one.
            if (stamp == unwritten_stamp_k) break;
            at = stamp - 1;
        }

        // A change that closes the chain folds onto nothing, as a chain no version closes does.
        if (closed && closing.kind == delta_kind_t::change_k) {
            if (chain.push_back(delta_view_t {closing}) != st::status_t::success_k)
                return status_t {status_code_t::out_of_memory_k, "No room to collect a key's changes"};
            closing = delta_view_t {delta_kind_t::remove_k, {}};
        }
        if (chain.empty()) {
            if (!closed) return lookup_result_t::missing_k;
            if (utilities::removes(closing.kind)) return lookup_result_t::removed_k;
            value = closing.bytes;
            return lookup_result_t::written_k;
        }
        std::reverse(chain.begin(), chain.end());
        std::span<byte_t const> const base = closing.kind == delta_kind_t::write_k ? closing.bytes
                                                                                   : std::span<byte_t const> {};
        auto [applied, apply_status] = fold_.apply(base, std::span<delta_view_t const> {chain}, arena);
        if (apply_status.failed()) return apply_status;
        value = applied;
        return lookup_result_t::written_k;
    }

    /** As @c find answers for a key whose newest version is a change, folding it with no I/O, so a chain reaching a
     *  level whose values sit apart from their key slots answers @c incomplete_k. */
    [[nodiscard]] lookup_result_t find_folded_(reading_t const& read, stored_key_t stored_key,
                                               stored_value_size_t& size) const noexcept {
        utilities::local_arena_t scratch;
        utilities::arena_allocator<byte_t> arena = scratch.allocator();
        std::span<byte_t const> value;
        auto bytes_of = [](auto const& run, std::uint64_t position,
                           std::span<byte_t const>& bytes) noexcept -> status_t {
            if constexpr (holds_values_inline_k) {
                key_slot_t const& slot = run.key_slot(position);
                bytes = std::span<byte_t const> {slot.data, slot.size};
                return {};
            }
            else return {status_code_t::io_k, "A change's value is read through a channel"};
        };
        expected<lookup_result_t> const folded = fold_versions_(read, stored_key, bytes_of, arena, value);
        if (!folded) return lookup_result_t::incomplete_k;
        if (*folded == lookup_result_t::written_k) size = static_cast<stored_value_size_t>(value.size());
        return *folded;
    }

    /** The bytes of the version at sorted @p position of @p run, read on @p channel into @p arena where they sit apart
     *  from the key slot. */
    template <typename run_type_>
    static status_t read_bytes_(io_channel_t& channel, run_type_ const& run, std::uint64_t position,
                                utilities::arena_allocator<byte_t>& arena, std::span<byte_t const>& bytes) noexcept {
        if constexpr (holds_values_inline_k) {
            key_slot_t const& slot = run.key_slot(position);
            bytes = std::span<byte_t const> {slot.data, slot.size};
            return {};
        }
        else {
            std::size_t const size = run.value_window(position).bytes;
            byte_t* const into = size != 0 ? arena.allocate(size) : nullptr;
            if (size != 0 && into == nullptr)
                return {status_code_t::out_of_memory_k, "No room to read a change's value"};
            if (status_t const read = run.read_value(channel, position, std::span<byte_t> {into, size}); read.failed())
                return read;
            bytes = std::span<byte_t const> {into, size};
            return {};
        }
    }

    /** As @c select answers for a key whose newest version is a change, reading the chain's values on @p channel. */
    status_t select_folded_(io_channel_t& channel, reading_t const& read, stored_key_t stored_key,
                            std::span<byte_t> into, stored_value_size_t& size) const noexcept {
        utilities::local_arena_t scratch;
        utilities::arena_allocator<byte_t> arena = scratch.allocator();
        std::span<byte_t const> value;
        auto bytes_of = [&](auto const& run, std::uint64_t position, std::span<byte_t const>& bytes) noexcept {
            return read_bytes_(channel, run, position, arena, bytes);
        };
        auto [folded, fold_status] = fold_versions_(read, stored_key, bytes_of, arena, value);
        if (fold_status.failed()) return fold_status;
        if (folded != lookup_result_t::written_k) return {status_code_t::not_found_k, "No version carries that key"};
        if (into.size() < value.size()) return {status_code_t::invalid_argument_k, "A buffer shorter than the value"};
        if (!value.empty()) std::memcpy(into.data(), value.data(), value.size());
        size = static_cast<stored_value_size_t>(value.size());
        return {};
    }

    /** Folds the keys of @p stored_keys that @p requests names, whose newest versions are changes, once a batched
     *  select's reads retired, in @p scratch, and hands each value to @p sink whole. */
    template <typename sink_type_>
    status_t select_folded_(io_channel_t& channel, reading_t const& read, ustore_arena_t scratch,
                            std::span<stored_key_t const> stored_keys, std::span<std::size_t const> requests,
                            std::span<lookup_result_t> results, sink_type_& sink) const noexcept {
        utilities::arena_allocator<byte_t> arena {scratch};
        for (std::size_t const request : requests) {
            std::span<byte_t const> value;
            auto bytes_of = [&](auto const& run, std::uint64_t position, std::span<byte_t const>& bytes) noexcept {
                return read_bytes_(channel, run, position, arena, bytes);
            };
            auto [folded, fold_status] = fold_versions_(read, stored_keys[request], bytes_of, arena, value);
            if (fold_status.failed()) return fold_status;
            results[request] = folded;
            if (folded == lookup_result_t::written_k) sink(request, std::uint64_t {0}, value);
        }
        return {};
    }

    /** Room for @p count deltas of one commit of its own. */
    expected<deltas_t> deltas_of_(std::size_t count) const noexcept {
        deltas_t deltas {constrained_allocator<keyed_delta_t, atomic_reference_> {runs_.constraint()}};
        if (deltas.resize(count) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room to hand a batch over"};
        return deltas;
    }

    /**
     *  @brief Keeps the last delta of every key of @p deltas, which a batch naming a
     *      key twice takes.
     *
     *  A batch is all writes, whose values sit back to back in arrival order, or all removals,
     *  which carry no bytes and are alike. So a key's latest write is the one whose bytes start
     *  last, or the longer one at a shared start, which only an empty value before it can take.
     */
    [[nodiscard]] static std::span<keyed_delta_t> last_of_each_key_(deltas_t& deltas) noexcept {
        utilities::sort_by_stored_key(std::span<keyed_delta_t> {deltas});
        std::size_t kept = 0;
        for (keyed_delta_t const& delta : deltas) {
            if (kept == 0 || deltas[kept - 1].stored_key != delta.stored_key) {
                deltas[kept++] = delta;
                continue;
            }
            std::span<byte_t const> const held = deltas[kept - 1].delta.bytes, offered = delta.delta.bytes;
            if (held.data() < offered.data() || (held.data() == offered.data() && held.size() < offered.size()))
                deltas[kept - 1] = delta;
        }
        return std::span<keyed_delta_t> {deltas.data(), kept};
    }

    /** Lands @p sealed as one commit of its own, flushing first when the memory runs are past their bounds, which
     *  the caller serializes with every other flush, and returns once it is published. */
    status_t commit_alone_(io_channel_t& channel, expected<sealed_commit_t> sealed) noexcept {
        if (!sealed) return sealed.status();
        // Merging by tier frees records without a write, so a flush is left to deltas and bytes
        // past their bounds.
        if (!sealed->empty() && memory_pressure(*sealed) == memory_pressure_t::stalled_k)
            if (status_t const merged = merge_memory_runs(); merged.failed()) return merged;
        if (!sealed->empty() && memory_pressure(*sealed) != memory_pressure_t::fits_k)
            if (status_t const flushed = flush(channel); flushed.failed()) return flushed;
        if (sealed->empty()) return {};
        memory_run_claim_t claimed;
        for (;;) {
            atomic_reference_<std::uint32_t const> const vacated {runs_.vacated()};
            std::uint32_t const observed = vacated.load(std::memory_order_acquire);
            status_t const taken = try_claim(claimed);
            if (taken.succeeded()) break;
            if (taken != status_code_t::capacity_exhausted_k) return taken;
            // A flush frees every published run's record; with none published, every record is out
            // with a commit.
            if (runs_.memory_shape().runs != 0) {
                if (status_t const flushed = flush(channel); flushed.failed()) return flushed;
            }
            else if (vacated.load(std::memory_order_acquire) == observed)
                waiter_type_ {}(&runs_.vacated(), observed, std::size_t {0});
        }
        return commit(*sealed, std::move(claimed));
    }

    static status_t prepare_alone_(void* context) noexcept {
        alone_t const& alone = *static_cast<alone_t const*>(context);
        alone.owner->prepare(*alone.sealed, std::move(*alone.claimed));
        return {};
    }

    static void publish_alone_(void* context, transaction_stamp_t stamp) noexcept {
        alone_t const& alone = *static_cast<alone_t const*>(context);
        alone.owner->publish(*alone.sealed, stamp);
    }

    /** Bytes the run laid out of @p key_slots key slots and @p values bytes of values takes at most, its trailer of
     *  zero-width stamps and its kinds counted at a byte each. */
    [[nodiscard]] static std::uint64_t ingested_run_bytes_(std::uint64_t key_slots, std::uint64_t values) noexcept {
        return sorted_run_layout_t::blob_bytes(key_slots, values, portable_alignment_k,
                                               key_slots + portable_alignment_k);
    }

    /** Whether a run of @p key_slots key slots and @p values bytes outgrows one slot. */
    [[nodiscard]] static bool ingested_run_overflows_(std::uint64_t key_slots, std::uint64_t values) noexcept {
        return ingested_run_bytes_(key_slots, values) > utilities::sorted_run_bytes_max_k;
    }

    /** Writes the rows @p ingested laid out as one run of stamp zero, and empties the layout for the next. */
    status_t write_ingested_run_(io_channel_t& channel, ingest_runs_t& ingested) noexcept {
        if (ingested.runs.reserve(ingested.runs.size() + 1) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "An ingest could not hold another run"};
        auto [written, write_status] = sorted_run_t::write(
            runs_.tree().volume(), channel, runs_.config().lsm_tree.collection, uuid_generator(),
            std::span<key_slot_t const> {ingested.drained.key_slots},
            std::span<std::uint64_t const> {ingested.drained.stamps},
            std::span<std::uint8_t const> {ingested.drained.kinds}, 0,
            std::span<byte_t const> {ingested.drained.stored_values},
            std::span<stored_value_offset_t const> {ingested.drained.value_offsets}, value_max_size_);
        if (write_status.failed()) return write_status;
        [[maybe_unused]] st::status_t const placed = ingested.runs.push_back(st::assume_reserved, std::move(written));
        ingested.drained.clear();
        return {};
    }

    /** Lays @p rows out behind what @p ingested laid out, writing a run each time one fills. */
    status_t lay_out_ingested_(io_channel_t& channel, std::span<keyed_delta_t const> rows,
                               ingest_runs_t& ingested) noexcept {
        stamped_deltas_t versions {constrained_allocator<stamped_delta_t, atomic_reference_> {runs_.constraint()}};
        for (std::size_t first = 0; first != rows.size();) {
            versions.clear();
            std::uint64_t values = ingested.drained.stored_values.size();
            std::size_t end = first;
            for (; end != rows.size(); ++end) {
                keyed_delta_t const& row = rows[end];
                std::uint64_t const slots = ingested.drained.key_slots.size() + versions.size() + 1;
                std::uint64_t const bytes = values + (is_fixed(value_layout_k) ? value_size_ : row.delta.bytes.size());
                if (ingested_run_overflows_(slots, bytes) && slots != 1) break;
                if (versions.push_back(stamped_delta_t {row.stored_key, 0, row.delta}) != st::status_t::success_k)
                    return {status_code_t::out_of_memory_k, "An ingest could not hold its rows"};
                values = bytes;
            }
            std::size_t const slots_before = ingested.drained.key_slots.size();
            std::size_t const values_before = ingested.drained.stored_values.size();
            if (status_t const laid = lay_out_versions_(std::span<stamped_delta_t const> {versions}, ingested.drained);
                laid.failed())
                return laid;
            // The rows view memory that outlives only this call, so their values are copied here.
            laid_out_values_t const rows_values = laid_out_values_(std::span<stamped_delta_t const> {versions},
                                                                   ingested.drained, slots_before);
            if (ingested.drained.stored_values.resize(st::assume_uninitialized,
                                                      static_cast<std::size_t>(values_before + rows_values.size())) !=
                st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A run could not hold its values"};
            rows_values.gather(0, std::span<byte_t> {ingested.drained.stored_values}.subspan(values_before));
            first = end;
            if (first != rows.size())
                if (status_t const written = write_ingested_run_(channel, ingested); written.failed()) return written;
        }
        return {};
    }

    /** Appends @p versions, in a run's order, to the key slots, stamps, kinds and value offsets
     *  @p drained lays a run out from, as a flush and an ingest both lay them, locating the values
     *  back to back past those @p drained holds: none for a layout holding them inline, one stride
     *  per key slot at a fixed width, and value offsets for a variable width alone, so
     *  @c laid_out_values_ reads the values where they lie. */
    status_t lay_out_versions_(std::span<stamped_delta_t const> versions, drained_t& drained) const noexcept {
        std::size_t const first = drained.key_slots.size();
        if (drained.key_slots.reserve(first + versions.size()) != st::status_t::success_k ||
            drained.stamps.reserve(first + versions.size()) != st::status_t::success_k ||
            drained.kinds.reserve(first + versions.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A run could not hold its key slots"};
        std::uint64_t added = 0;
        for (stamped_delta_t const& version : versions) added += version.delta.bytes.size();
        if constexpr (is_fixed(value_layout_k)) added = std::uint64_t {versions.size()} * value_size_;
        std::uint64_t written = holds_values_inline_k ? 0 : drained.stored_values.size();
        if constexpr (sorted_run_layout_t::carries_value_offsets_k) {
            if (written + added > std::numeric_limits<stored_value_offset_t>::max())
                return {status_code_t::capacity_exhausted_k, "A run's values outgrow its value offsets"};
            if (drained.value_offsets.resize(first + versions.size() + 1) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A run could not hold its value offsets"};
        }

        for (std::size_t index = 0; index != versions.size(); ++index) {
            stamped_delta_t const& version = versions[index];
            std::span<byte_t const> const bytes = version.delta.bytes;
            key_slot_t built {};
            built.stored_key = version.stored_key;
            if constexpr (holds_values_inline_k) {
                // A merge may compose changes wider than any one of them, which a key slot cannot hold.
                if (bytes.size() > sizeof(built.data))
                    return {status_code_t::capacity_exhausted_k, "A folded change outgrows its key slot"};
                if (!bytes.empty()) std::memcpy(built.data, bytes.data(), bytes.size());
                built.size = static_cast<stored_value_size_t>(bytes.size());
            }
            else {
                std::uint64_t const stride = is_fixed(value_layout_k) ? value_size_ : bytes.size();
                if (bytes.size() > stride)
                    return {status_code_t::capacity_exhausted_k, "A value outgrows its fixed width"};
                if constexpr (sorted_run_layout_t::carries_value_offsets_k)
                    drained.value_offsets[first + index] = static_cast<stored_value_offset_t>(written);
                written += stride;
            }
            [[maybe_unused]] st::status_t const placed = drained.key_slots.push_back(std::move(built));
            [[maybe_unused]] st::status_t const stamped = drained.stamps.push_back(std::uint64_t {version.stamp});
            [[maybe_unused]] st::status_t const kinded = drained.kinds.push_back(
                static_cast<std::uint8_t>(version.delta.kind));
        }
        if constexpr (sorted_run_layout_t::carries_value_offsets_k)
            drained.value_offsets[first + versions.size()] = static_cast<stored_value_offset_t>(written);
        return {};
    }

    /** The values of @p versions as @c lay_out_versions_ located them in @p drained from key slot
     *  @p first on. */
    [[nodiscard]] laid_out_values_t laid_out_values_(std::span<stamped_delta_t const> versions,
                                                     drained_t const& drained, std::size_t first) const noexcept {
        laid_out_values_t values;
        values.versions = versions;
        if constexpr (sorted_run_layout_t::carries_value_offsets_k)
            values.value_offsets = std::span<stored_value_offset_t const> {drained.value_offsets}.subspan(
                first, versions.size() + 1);
        if constexpr (is_fixed(value_layout_k)) values.stride = value_size_;
        return values;
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The tree, memory runs and view this store publishes, and every merge, flush and compaction of them. */
    published_runs_t runs_;

    /** The values of versions point reads paid I/O for, never cleared. */
    value_cache_t cache_;

    /** Bytes every value takes, zero for a layout whose values differ in width. */
    stored_value_size_t value_size_ = 0;

    /** Bytes the largest value this collection admits takes. */
    stored_value_size_t value_max_size_ = 0;

    /** What a change delta does, which reads and merges fold with. */
    [[no_unique_address]] fold_t fold_ {};

    /** What composes the changes of a merge on a device, which compactions hand the tree. */
    [[no_unique_address]] device_fold_t device_fold_ {};

#pragma endregion Private Members
};

#pragma endregion Store

#pragma endregion Store

} // namespace unum::ustore::blobs

#include "ustore/blobs/nvme_store_cursor.hpp" // `nvme_store_cursor`
