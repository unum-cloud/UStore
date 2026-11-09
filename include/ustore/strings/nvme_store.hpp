/**
 *  @file include/ustore/strings/nvme_store.hpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief The NVMe store of a strings collection, keyed by whole strings: strings memory runs in front of an LSM tree
 *      whose runs carry the tails of their strings, and the strings one database transaction writes and watches.
 *
 *  Every string is a key of its own, mapped onto one 16-byte key of another collection. A memory
 *  run keeps it whole on its key tape, and a run file keeps its 16-byte head and its 16-byte value
 *  in a 32-byte cell and the bytes past the head in the run's tails, which are read into memory
 *  with the index, so a lookup binary-searches the heads and compares tails only between equal
 *  heads, and answers the value from the cell, with no I/O. Two strings sharing their leading 15
 *  bytes stay two keys everywhere: in a commit's watches, in a merge, which keeps the newest
 *  version of each, and in a sorted level, whose runs never split the versions of one string, so a
 *  level finds a string in one run.
 *
 *  The store lives in a database beside the blobs stores and draws its stamps from the same ledger. The database keys
 *  its own transactions by 16 bytes, so the strings of a transaction ride in a @c nvme_transaction, which joins the
 *  database transaction as a share: its owner seals it before the commit, and the leader checks its watches, lays out
 *  its views and publishes them under the stamp the rest of the commit lands under.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm>   // `std::max`, `std::min`
#include <compare>     // `std::strong_ordering`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`, `std::as_bytes`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_same_v`
#include <utility>     // `std::move`

#include <smashtable/basic_hash_table.hpp> // `st::hash_map`
#include <smashtable/basic_vector.hpp>     // `st::vector`

#include "ustore/blobs/nvme_published_runs.hpp" // `published_runs`, `memory_pressure_t`, `memory_shape_t`
#include "ustore/blobs/nvme_store.hpp"          // `commit_ledger`, `stamp_pin`, `lookup_result_t`
#include "ustore/blobs/nvme_transactions.hpp"   // `transaction_share_t`
#include "ustore/blobs/nvme_tree.hpp"           // `collection_config_t`, `lsm_tree_publication_hook_t`
#include "ustore/strings/string_keys.hpp"       // `head_of`, `head_length_of`, `tail_of`, `head_prefix_bytes_k`
#include "ustore/strings/nvme_memory_run.hpp"   // `memory_run`, `sealed_deltas`, `kept_versions`
#include "ustore/utilities/folds.hpp"           // `last_write_fold_t`, `delta_view_t`
#include "ustore/utilities/ram_buffers.hpp"     // `local_arena_t`, `strings_kit_concept`
#include "ustore/utilities/types.hpp"           // `status_t`, `expected`, `byte_t`

namespace unum::ustore::strings {

using blobs::collection_config_t;
using blobs::collection_id_hash_t;
using blobs::collection_id_t;
using blobs::commit_ledger;
using blobs::compaction_outcome_t;
using blobs::head_stamp_k;
using blobs::invalid_transaction_stamp_k;
using blobs::key_positions_t;
using blobs::key_slot;
using blobs::key_tails_column_t;
using blobs::key_tails_t;
using blobs::lookup_result_t;
using blobs::lsm_level_order_t;
using blobs::lsm_tree_publication_hook_t;
using blobs::lsm_tree_run_record_t;
using blobs::memory_pressure_t;
using blobs::memory_shape_t;
using blobs::published_runs;
using blobs::scan_page_keys;
using blobs::stamp_pin;
using blobs::stored_value_size_t;
using blobs::transaction_share_t;
using blobs::value_layout_t;
using utilities::constrained_allocator;
using utilities::io_channel_t;
using utilities::outside_executor_k;
using utilities::resource_constraint;

#pragma region Keys

/** Orders the string of head @p first_head and tail @p first_tail against another, by heads and then by tails, which
 *  unsigned byte order of the whole strings agrees with. */
inline std::strong_ordering order_keys(st::key128_t const& first_head, std::string_view first_tail,
                                       st::key128_t const& second_head, std::string_view second_tail) noexcept {
    std::strong_ordering const heads = first_head <=> second_head;
    if (heads != 0) return heads;
    return first_tail.compare(second_tail) <=> 0;
}

/** The string whose head is @p head and whose tail is @p tail, copied into @p whole, which @p tail must not view;
 *  a string the head holds whole has no tail. */
template <typename buffer_type_>
status_t string_of_key(st::key128_t const& head, std::string_view tail, buffer_type_& whole,
                       std::string_view& text) noexcept {
    std::size_t const length = head_length_of(head);
    std::size_t const leading = length <= head_prefix_bytes_k ? length : head_prefix_bytes_k;
    if (whole.resize(leading + tail.size()) != st::status_t::success_k)
        return {status_code_t::out_of_memory_k, "No room to put a string back together"};
    std::byte head_bytes[sizeof(st::key128_t)];
    head.to_bytes(head_bytes);
    if (leading != 0) std::memcpy(whole.data(), head_bytes, leading);
    if (!tail.empty()) std::memcpy(whole.data() + leading, tail.data(), tail.size());
    text = std::string_view {whole.begin(), whole.end()};
    return {};
}

#pragma endregion Keys

#pragma region Pages

/** Strings one page of a walk lists at most, as many as a page of a scan of 16-byte keys holds. */
inline constexpr std::size_t strings_page_k = scan_page_keys<std::string_view>();

/** One string a page of a walk lists, located in the page's texts. */
struct listed_string_t {

    /** Where its bytes start in the page's texts. */
    std::size_t offset = 0;

    /** Bytes it takes. */
    std::size_t length = 0;

    /** Whether its newest version carries a value or a removal. */
    lookup_result_t state = lookup_result_t::missing_k;

    /** The 16 bytes of the value as they were written, zero for a removal. */
    ustore_key_t value {};
};

/** The strings one page of a walk read, in order, copied out of the runs, so they outlive the epoch they were read in;
 *  reused from page to page. */
struct strings_page_t {

    /** The strings back to back. */
    st::vector<char> texts;

    /** Where each string sits in @c texts, in order. */
    st::vector<listed_string_t> listed;

    /** A string put back together from a head and a tail before it lands in @c texts. */
    st::vector<char> whole;

    /** Strings the page lists. */
    [[nodiscard]] std::size_t size() const noexcept { return listed.size(); }

    /** The string at @p index. */
    [[nodiscard]] std::string_view text(std::size_t index) const noexcept {
        return std::string_view {texts.data() + listed[index].offset, listed[index].length};
    }
};

#pragma endregion Pages

#pragma region Store

/**
 *  @brief Strings memory runs and an LSM tree of strings runs behind one published view, keyed by whole strings.
 *
 *  A reader announces itself in its ledger's epochs, loads the published stamp and then the view,
 *  and walks immutable runs with no lock. A committer seals its strings into one memory run on its
 *  own thread, and the ledger's leader publishes a view naming it ahead of the others. Merges of
 *  memory runs, flushes, compactions and truncations follow @c blobs::nvme_store, and are
 *  serialized by the database that owns the store. A string's newest version answers it, a removal
 *  included, and every version a pinned reader or a mount needs survives a merge.
 *
 *  @tparam atomic_reference_ The reference the published view, the budget and the ledger own their words through.
 *  @tparam waiter_type_ What a core waiting on a read, a barrier or the ledger does between attempts, before it parks.
 *  @tparam strings_kit_type_ The StringZilla kernels every seal sorts and every transaction hashes
 *      its strings through.
 */
template <typename volume_type_, template <typename> typename atomic_reference_, typename waiter_type_,
          utilities::strings_kit_concept strings_kit_type_>
class nvme_store {
  public:

#pragma region Types

    /** The StringZilla kernels every seal sorts and every transaction hashes
     *  its strings through. */
    using strings_kit_t = strings_kit_type_;

    /** The cell every run of this store holds: the head of a string and the 16 bytes of its value, in runs that carry
     *  the tails of their strings, which tell the strings of one head apart. */
    using key_slot_t = key_slot<st::key128_t, value_layout_t::uuid_k, key_tails_t::carried_k>;

    /** The volume every run of this store lives on. */
    using volume_t = volume_type_;

    /** What a delta of this store does, which is always a whole value or a removal. */
    using fold_t = utilities::last_write_fold_t;

    /** The key a caller addresses this store by. */
    using stored_key_t = std::string_view;

    /** The stamps this store's writes carry and its readers pin. */
    using ledger_t = commit_ledger<atomic_reference_, waiter_type_>;

    /** One stamp a reader or a view of this store holds against the ledger. */
    using pin_t = stamp_pin<atomic_reference_, waiter_type_>;

    /** The budget the memory runs are charged to. */
    using constraint_t = resource_constraint<atomic_reference_>;

    /** Every column of a memory run comes from that budget. */
    using allocator_t = constrained_allocator<byte_t, atomic_reference_>;

    /** One commit's strings, sorted and copied, before they are stamped. */
    using sealed_deltas_t = sealed_deltas<allocator_t>;

    /** One commit's strings, stamped, which nothing changes once a view names them. */
    using memory_run_t = memory_run<allocator_t>;

    /** The tree, memory runs and view this store publishes, and every merge, flush and compaction of them. */
    using published_runs_t = published_runs<key_slot_t, memory_run_t, volume_t, atomic_reference_, waiter_type_>;

    /** The tree the flushed strings live in. */
    using tree_t = typename published_runs_t::tree_t;

    /** One level set of that tree, which a view names. */
    using level_set_t = typename tree_t::level_set_t;

    /** One immutable run of a level. */
    using sorted_run_t = typename tree_t::sorted_run_t;

    /** The database's pool the runs of that tree live in. */
    using run_pool_t = typename tree_t::run_pool_t;

    /** Everything a reader of this store needs, which nothing changes once it is published. */
    using view_t = typename published_runs_t::view_t;

    /** One delta a commit hands over: the string, its kind, and its bytes. */
    using keyed_delta_t = keyed_delta<std::string_view>;

    /** One version a merge or a flush carries over from a memory run. */
    using stamped_delta_t = stamped_delta<std::string_view>;

    /** One commit's strings for this store, sealed off every shared path by its committer, and the run the leader
     *  stamps them into, both charged to the store's budget. */
    struct sealed_commit_t {

        /** The deltas, sorted and copied, which the leader moves into a run of the store's pool
         *  once it draws the stamp. */
        sealed_deltas_t deltas;

        /** Whether the commit has no string or range tombstone for this store, adding no run. */
        [[nodiscard]] bool empty() const noexcept { return deltas.size() == 0 && deltas.range_tombstones().empty(); }
    };

    /** The strings one range removal hides. */
    using range_t = range_tombstone<std::string_view>;

    /** One reader inside the ledger's epochs, announced until it dies, which keeps every view
     *  loaded meanwhile. */
    using reader_t = typename published_runs_t::reader_t;

    /** The view a read walks and the stamp it answers at, which every read of a call may share. */
    using reading_t = typename published_runs_t::reading_t;

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
     *  @param[in] config The bounds of the memory runs and the shape of the tree below.
     *  @param[in,out] constraint The budget the memory runs are charged to.
     *  @param[in,out] ledger The stamps the store's writes carry and its readers pin, outliving the store.
     *  @param[in,out] runs The database's pool the tree's runs live in, outliving the store.
     */
    static expected<nvme_store> mount(volume_t& volume, collection_config_t const& config, constraint_t& constraint,
                                      ledger_t& ledger, run_pool_t& runs) noexcept {
        nvme_store mounted;
        if (status_t const mounted_runs = mounted.runs_.mount(volume, config, constraint, ledger, runs);
            mounted_runs.failed())
            return mounted_runs;
        return mounted;
    }

    /** Returns every run to the volume and forgets every memory run, which destroys the data and which only a store no
     *  reader reaches and no manifest names may. */
    void destroy() noexcept { runs_.destroy(); }

#pragma endregion Construction

#pragma region Shape

    /** The bounds this store was mounted with. */
    [[nodiscard]] collection_config_t const& config() const noexcept { return runs_.config(); }

    /** The tree the flushed strings live in, which a database compacts and names in its schema. */
    [[nodiscard]] tree_t const& tree() const noexcept { return runs_.tree(); }

    /** Bytes every value this store admits takes: one 16-byte key. */
    [[nodiscard]] stored_value_size_t value_max_size() const noexcept { return key_slot_t::size; }

    /** How many memory runs the published view names, and how many commits they hold. */
    [[nodiscard]] memory_shape_t memory_shape() const noexcept { return runs_.memory_shape(); }

    /** Deltas the memory runs hold plus key slots every level holds, one string counted once per run holding it. */
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
     *  @brief Whether anything carries @p text, copying its 16-byte value into @p value from a memory run or from a
     *      cell, with no I/O.
     *  @param[in] text The string to look up.
     *  @param[out] value The value, written only for @c written_k.
     *  @param[in] stamp The stamp a reader is pinned at, or @c head_stamp_k for the published one.
     *  @param[in] thread_index The executor thread reading, which announces on its own epoch slot.
     */
    [[nodiscard]] lookup_result_t find(std::string_view text, std::span<byte_t, key_slot_t::size> value,
                                       transaction_stamp_t stamp = head_stamp_k,
                                       std::size_t thread_index = outside_executor_k) const noexcept {
        reader_t const inside = runs_.enter(thread_index);
        return find(runs_.reading(stamp), text, value);
    }

    /** As @c find above, through @p read, which a caller inside the ledger's epochs took for every
     *  read of a call. */
    [[nodiscard]] lookup_result_t find(reading_t const& read, std::string_view text,
                                       std::span<byte_t, key_slot_t::size> value) const noexcept {
        found_t found;
        if (!newest_(*read.view, text, read.stamp, found)) return lookup_result_t::missing_k;
        if (found.delta.kind == delta_kind_t::remove_k || hidden_(*read.view, text, read.stamp, found))
            return lookup_result_t::removed_k;
        std::memcpy(value.data(), found.run ? found.run->value(found.position).data() : found.delta.bytes.data(),
                    key_slot_t::size);
        return lookup_result_t::written_k;
    }

    /** Whether anything carries @p text, answering in @p size the 16 bytes its value takes, or zero for anything but
     *  @c written_k, as a database asks of every store. */
    [[nodiscard]] lookup_result_t find(std::string_view text, stored_value_size_t& size,
                                       transaction_stamp_t stamp = head_stamp_k,
                                       std::size_t thread_index = outside_executor_k) const noexcept {
        byte_t value[key_slot_t::size];
        lookup_result_t const found = find(text, value, stamp, thread_index);
        size = found == lookup_result_t::written_k ? key_slot_t::size : 0;
        return found;
    }

    /**
     *  @brief Copies the 16-byte value of @p text into @p into, as a database asks of every store, with no I/O.
     *  @param[in] text The string to look up.
     *  @param[out] into Room for the value's bytes.
     *  @param[out] size Bytes written, which is zero for every failure.
     *  @param[in] stamp The stamp a reader is pinned at, or @c head_stamp_k for the published one.
     *  @param[in] thread_index The executor thread reading, which announces on its own epoch slot.
     *  @return @c not_found_k for a miss or a removal, @c invalid_argument_k for a buffer shorter than 16 bytes.
     */
    status_t select(io_channel_t&, std::string_view text, std::span<byte_t> into, stored_value_size_t& size,
                    transaction_stamp_t stamp = head_stamp_k, std::size_t thread_index = outside_executor_k) noexcept {
        size = 0;
        if (into.size() < key_slot_t::size) return {status_code_t::invalid_argument_k, "A buffer shorter than a key"};
        if (find(text, into.first<key_slot_t::size>(), stamp, thread_index) != lookup_result_t::written_k)
            return {status_code_t::not_found_k, "No version carries that string"};
        size = key_slot_t::size;
        return {};
    }

    /**
     *  @brief Reads into @p page up to @c strings_page_k strings starting with @p prefix and not below @p from, in
     *      order, each with what its newest version at or below @p stamp carries, a removal included.
     *  @param[in] prefix What every string of the page starts with.
     *  @param[in] from The first string the page may hold.
     *  @param[in] stamp A stamp a reader pins, whose versions every merge keeps, so a walk reads every page at it.
     *  @param[in] thread_index The executor thread reading, which announces on its own epoch slot.
     *  @param[out] page The strings read, each with what its newest version carries.
     *
     *  Every run gets a cursor over its window, newest first: the memory runs, then level zero youngest first, then
     *  the deeper levels. The page takes the smallest key among them, answers it from the first cursor holding a
     *  version at or below the stamp, and moves every cursor past it. A walk reads a page inside the ledger's epochs
     *  at a time, so a commit waits for one page at most, and starts the next past the last string of the page.
     */
    status_t read_page(std::string_view prefix, std::string_view from, transaction_stamp_t stamp,
                       std::size_t thread_index, strings_page_t& page) const noexcept {
        reader_t const inside = runs_.enter(thread_index);
        return read_page(reading_t {runs_.view(), stamp}, prefix, from, page);
    }

    /** As @c read_page above, through @p read, which a caller inside the ledger's epochs took for
     *  every read of a call. */
    status_t read_page(reading_t const& read, std::string_view prefix, std::string_view from,
                       strings_page_t& page) const noexcept {
        page.texts.clear();
        page.listed.clear();
        view_t const& view = *read.view;
        transaction_stamp_t const stamp = read.stamp;
        level_set_t const& levels = *view.levels;
        std::size_t runs = view.memory_runs.runs().size();
        for (std::size_t number = 0; number != levels.size(); ++number) runs += levels.level(number).size();
        st::vector<cursor_t> cursors;
        if (cursors.reserve(runs) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A walk could not hold a cursor per run"};
        for (memory_run_t const* run : view.memory_runs.runs()) {
            cursor_t cursor;
            cursor.memory = run;
            cursor.end = run->prefix_end(prefix);
            cursor.position = std::min<std::uint64_t>(run->lower_bound(from), cursor.end);
            [[maybe_unused]] st::status_t const placed = cursors.push_back(std::move(cursor));
        }
        for (std::size_t number = 0; number != levels.size(); ++number) {
            auto const& level = levels.level(number);
            for (std::size_t index = level.size(); index-- != 0;) {
                sorted_run_t const& run = level.run(index);
                cursor_t cursor;
                cursor.level = &run;
                cursor.level_number = static_cast<std::uint32_t>(number);
                cursor.run_index = static_cast<std::uint32_t>(index);
                auto [end, end_status] = prefix_end_in_(run, prefix);
                if (end_status.failed()) return end_status;
                cursor.end = end;
                cursor.position = std::min(run.lower_bound(from), cursor.end);
                [[maybe_unused]] st::status_t const placed = cursors.push_back(std::move(cursor));
            }
        }

        while (page.listed.size() != strings_page_k) {
            std::size_t smallest = cursors.size();
            for (std::size_t index = 0; index != cursors.size(); ++index) {
                if (cursors[index].position == cursors[index].end) continue;
                if (smallest != cursors.size() &&
                    order_keys(cursor_head_(cursors[index]), cursor_tail_(cursors[index]),
                               cursor_head_(cursors[smallest]), cursor_tail_(cursors[smallest])) >= 0)
                    continue;
                smallest = index;
            }
            if (smallest == cursors.size()) return {};

            st::key128_t const head = cursor_head_(cursors[smallest]);
            std::string_view const tail = cursor_tail_(cursors[smallest]);
            if (status_t const listed = list_key_(std::span<cursor_t> {cursors}, head, tail, stamp, view, page);
                listed.failed())
                return listed;
        }
        return {};
    }

    /**
     *  @brief Reads into @p page up to @c strings_page_k strings starting with @p prefix and below
     *      @p below, or every one of the prefix for an empty @p below, largest first, as
     *      @c read_page reads them upwards.
     *
     *  Every cursor walks its window down from its end: the largest string any cursor ends at is
     *  listed from the first version of it each run holds, newest first, and every cursor holding
     *  it then ends below it, so a page reads about its own strings per run. A walk passes the
     *  page's last string as the next one's @p below.
     */
    status_t read_page_down(std::string_view prefix, std::string_view below, transaction_stamp_t stamp,
                            std::size_t thread_index, strings_page_t& page) const noexcept {
        reader_t const inside = runs_.enter(thread_index);
        return read_page_down(reading_t {runs_.view(), stamp}, prefix, below, page);
    }

    /** As @c read_page_down above, through @p read, which a caller inside the ledger's epochs took
     *  for every read of a call. */
    status_t read_page_down(reading_t const& read, std::string_view prefix, std::string_view below,
                            strings_page_t& page) const noexcept {
        page.texts.clear();
        page.listed.clear();
        view_t const& view = *read.view;
        transaction_stamp_t const stamp = read.stamp;
        level_set_t const& levels = *view.levels;
        std::size_t runs = view.memory_runs.runs().size();
        for (std::size_t number = 0; number != levels.size(); ++number) runs += levels.level(number).size();
        st::vector<cursor_t> cursors;
        st::vector<std::uint64_t> starts;
        if (cursors.reserve(runs) != st::status_t::success_k || starts.resize(runs) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A walk could not hold a cursor per run"};
        for (memory_run_t const* run : view.memory_runs.runs()) {
            cursor_t cursor;
            cursor.memory = run;
            cursor.begin = run->lower_bound(prefix);
            cursor.end = run->prefix_end(prefix);
            if (!below.empty()) cursor.end = std::min<std::uint64_t>(cursor.end, run->lower_bound(below));
            [[maybe_unused]] st::status_t const placed = cursors.push_back(std::move(cursor));
        }
        for (std::size_t number = 0; number != levels.size(); ++number) {
            auto const& level = levels.level(number);
            for (std::size_t index = level.size(); index-- != 0;) {
                sorted_run_t const& run = level.run(index);
                cursor_t cursor;
                cursor.level = &run;
                cursor.level_number = static_cast<std::uint32_t>(number);
                cursor.run_index = static_cast<std::uint32_t>(index);
                auto [end, end_status] = prefix_end_in_(run, prefix);
                if (end_status.failed()) return end_status;
                cursor.begin = run.lower_bound(prefix);
                cursor.end = below.empty() ? end : std::min(end, run.lower_bound(below));
                [[maybe_unused]] st::status_t const placed = cursors.push_back(std::move(cursor));
            }
        }

        auto const head_below = [](cursor_t const& cursor, std::uint64_t position) noexcept {
            return cursor.memory ? head_at_(*cursor.memory, position) : head_at_(*cursor.level, position);
        };
        auto const tail_below = [](cursor_t const& cursor, std::uint64_t position) noexcept {
            return cursor.memory ? tail_at_(*cursor.memory, position) : tail_at_(*cursor.level, position);
        };
        while (page.listed.size() != strings_page_k) {
            std::size_t largest = cursors.size();
            for (std::size_t index = 0; index != cursors.size(); ++index) {
                cursor_t const& cursor = cursors[index];
                if (cursor.end <= cursor.begin) continue;
                if (largest != cursors.size() &&
                    order_keys(head_below(cursor, cursor.end - 1), tail_below(cursor, cursor.end - 1),
                               head_below(cursors[largest], cursors[largest].end - 1),
                               tail_below(cursors[largest], cursors[largest].end - 1)) <= 0)
                    continue;
                largest = index;
            }
            if (largest == cursors.size()) return {};

            cursor_t const& top = cursors[largest];
            st::key128_t const head = head_below(top, top.end - 1);
            std::string_view const tail = tail_below(top, top.end - 1);
            // Each run holding the string starts at its first version, the listing reading on.
            for (std::size_t index = 0; index != cursors.size(); ++index) {
                cursor_t& cursor = cursors[index];
                std::uint64_t start = cursor.end;
                while (start > cursor.begin &&
                       order_keys(head_below(cursor, start - 1), tail_below(cursor, start - 1), head, tail) == 0)
                    --start;
                starts[index] = start;
                cursor.position = start;
            }
            if (status_t const listed = list_key_(std::span<cursor_t> {cursors}, head, tail, stamp, view, page);
                listed.failed())
                return listed;
            for (std::size_t index = 0; index != cursors.size(); ++index) cursors[index].end = starts[index];
        }
        return {};
    }

#pragma endregion Lookups

#pragma region Writes

    /** Whether a written value of @p size bytes is one this store admits: one 16-byte key. */
    [[nodiscard]] static bool admits(std::size_t size) noexcept { return size == key_slot_t::size; }

    /**
     *  @brief Sorts and copies @p deltas into one commit's sealed deltas for this store, on the committer's thread and
     *      off every shared path, and allocates the run they become; no deltas seal into no run.
     *  @param[in] deltas One delta per string, which this sorts in place: writes of one 16-byte key and removals.
     *  @param[in] ranges The ranges of strings the commit removes, each hiding every older version.
     */
    expected<sealed_commit_t> seal(std::span<keyed_delta_t> deltas,
                                   std::span<range_t const> ranges = {}) const noexcept {
        for (keyed_delta_t const& each : deltas) {
            if (each.delta.kind == delta_kind_t::change_k)
                return status_t {status_code_t::invalid_argument_k, "A string takes whole values and removals"};
            if (each.delta.kind == delta_kind_t::write_k && !admits(each.delta.bytes.size()))
                return status_t {status_code_t::malformed_k, "A string maps onto one 16-byte key"};
        }
        sealed_commit_t sealed;
        if (deltas.empty() && ranges.empty()) return sealed;
        auto [sealed_deltas, seal_status] = sealed_deltas_t::seal(deltas, strings_kit_type_::sort_order, ranges,
                                                                  allocator_t {runs_.constraint()});
        if (seal_status.failed()) return seal_status;
        sealed.deltas = std::move(sealed_deltas);
        return sealed;
    }

    /** Seals one value per string of @p texts, a repeated string taking its last value, out of @p values cut by
     *  @p sizes, each of which names 16 bytes. */
    expected<sealed_commit_t> seal_writes(std::span<std::string_view const> texts, std::span<byte_t const> values,
                                          std::span<stored_value_size_t const> sizes) const noexcept {
        if (sizes.size() != texts.size())
            return status_t {status_code_t::invalid_argument_k, "A batched insert names the size of every value"};
        std::uint64_t total = 0;
        for (stored_value_size_t const size : sizes) total += size;
        if (total != values.size())
            return status_t {status_code_t::invalid_argument_k, "The named sizes do not add up to the values"};
        auto [deltas, deltas_status] = deltas_of_(texts.size());
        if (deltas_status.failed()) return deltas_status;
        std::size_t offset = 0;
        for (std::size_t index = 0; index != texts.size(); ++index) {
            deltas[index] = keyed_delta_t {texts[index],
                                           delta_view_t {delta_kind_t::write_k, values.subspan(offset, sizes[index])}};
            offset += sizes[index];
        }
        return seal_last_of_each_(deltas);
    }

    /** Seals a removal of every string of @p texts. */
    expected<sealed_commit_t> seal_removals(std::span<std::string_view const> texts) const noexcept {
        auto [deltas, deltas_status] = deltas_of_(texts.size());
        if (deltas_status.failed()) return deltas_status;
        for (std::size_t index = 0; index != texts.size(); ++index)
            deltas[index] = keyed_delta_t {texts[index], delta_view_t {delta_kind_t::remove_k, {}}};
        return seal_last_of_each_(deltas);
    }

    /** How full the memory runs of the published view would be with @p sealed beside them. */
    [[nodiscard]] memory_pressure_t memory_pressure(sealed_commit_t const& sealed) const noexcept {
        return runs_.memory_pressure(sealed.deltas.size(), sealed.deltas.keys_bytes() + sealed.deltas.values_bytes() +
                                                               sealed.deltas.range_tombstones().bytes());
    }

    /** Claims a memory-run record and the room of the view naming its run, or answers
     *  @c capacity_exhausted_k while every record is taken, which the claimer meets by flushing
     *  or by waiting on @c vacated. */
    status_t try_claim(blobs::memory_run_claim_t& claimed) noexcept { return runs_.try_claim(claimed); }

    /** Takes back what @p claimed holds, which this store claimed and nobody prepared. */
    void give_back(blobs::memory_run_claim_t& claimed) noexcept { runs_.give_back(claimed); }

    /** The count of memory-run records vacated so far, which a claimer finding every one
     *  taken waits on. */
    [[nodiscard]] std::uint32_t const& vacated() const noexcept { return runs_.vacated(); }

    /**
     *  @brief Lays out the view naming @p sealed's run ahead of the current ones, out of the room
     *      @p claimed holds, so @c publish cannot fail.
     *  @note Called by the thread holding the ledger's leader word, which then publishes or forgets
     *      it before it lets the word go.
     */
    void prepare(sealed_commit_t& sealed, blobs::memory_run_claim_t&& claimed) noexcept {
        if (sealed.empty()) runs_.forget_prepared();
        else runs_.prepare(std::move(claimed));
    }

    /** Stamps @p sealed's run with @p stamp, publishes the view @c prepare laid out, and takes the run over, which the
     *  leader does before it publishes @p stamp itself, so a reader at the stamp finds the run. */
    void publish(sealed_commit_t& sealed, transaction_stamp_t stamp) noexcept {
        if (sealed.empty()) return;
        expected<memory_run_t> stamped = memory_run_t::stamped(std::move(sealed.deltas), stamp,
                                                               allocator_t {runs_.constraint()});
        runs_.publish(stamped ? std::move(*stamped) : memory_run_t {}, stamp);
    }

    /** Drops the view @c prepare laid out, which the leader does for a commit it then refuses. */
    void forget_prepared() noexcept { runs_.forget_prepared(); }

    /** Whether a commit newer than @p from wrote @p text, which only the string's newest version decides. */
    [[nodiscard]] bool written_since(std::string_view text, transaction_stamp_t from) const noexcept {
        reader_t const inside = runs_.enter();
        view_t const& view = *runs_.view();
        // Memory runs sit newest first and above every level, so one no newer than the watch ends the search.
        for (memory_run_t const* run : view.memory_runs.runs()) {
            if (run->newest_stamp() <= from) return false;
            if (run->range_tombstones().overlaps_since(text, text, from)) return true;
            std::size_t const position = run->lower_bound(text);
            if (position != run->size() && run->stored_key(position) == text) return run->stamp(position) > from;
        }
        if (any_level_run_(*view.levels, [&](sorted_run_t const& run) noexcept {
                return run.range_tombstones().overlaps_since(text, text, from);
            }))
            return true;
        found_t found;
        if (!newest_in_levels_(*view.levels, text, head_stamp_k, found)) return false;
        return found.run->stamp(found.position) > from;
    }

    /** Whether a commit newer than @p from wrote any string from @p lower to @p upper inclusive, which a walk over them
     *  watches, so a string written where it found none conflicts too. */
    [[nodiscard]] bool range_written_since(std::string_view lower, std::string_view upper,
                                           transaction_stamp_t from) const noexcept {
        if (upper < lower) return false;
        reader_t const inside = runs_.enter();
        view_t const& view = *runs_.view();
        if (view.memory_runs.range_written_since(lower, upper, from)) return true;
        return any_level_run_(*view.levels, [&](sorted_run_t const& run) noexcept {
            if (run.newest_stamp() <= from) return false;
            if (run.range_tombstones().overlaps_since(lower, upper, from)) return true;
            for (std::uint64_t position = run.lower_bound(lower);
                 position != run.population_count() && run.key(position) <= upper; ++position)
                if (run.stamp(position) > from) return true;
            return false;
        });
    }

    /** Whether a commit newer than @p from wrote any string starting with @p prefix and not below @p lower, which a
     *  walk to the end of the prefix watches. */
    [[nodiscard]] bool prefix_written_since(std::string_view prefix, std::string_view lower,
                                            transaction_stamp_t from) const noexcept {
        std::string_view const first = std::max(lower, prefix);
        reader_t const inside = runs_.enter();
        view_t const& view = *runs_.view();
        for (memory_run_t const* run : view.memory_runs.runs()) {
            if (run->newest_stamp() <= from) break;
            if (prefix_removed_since_(run->range_tombstones(), prefix, first, from)) return true;
            std::size_t const end = run->prefix_end(prefix);
            for (std::size_t position = run->lower_bound(first); position < end; ++position)
                if (run->stamp(position) > from) return true;
        }
        return any_level_run_(*view.levels, [&](sorted_run_t const& run) noexcept {
            if (run.newest_stamp() <= from) return false;
            if (prefix_removed_since_(run.range_tombstones(), prefix, first, from)) return true;
            auto [end, end_status] = prefix_end_in_(run, prefix);
            // Without room to bound the prefix, the commit is told it conflicts, which
            // a retry clears.
            if (end_status.failed()) return true;
            for (std::uint64_t position = run.lower_bound(first); position < end; ++position)
                if (run.stamp(position) > from) return true;
            return false;
        });
    }

    /** Opens the runs @p runs names as this store's tree, with the stamps ingests assigned, which a
     *  mount does before anything reads. */
    status_t restore(io_channel_t& channel, std::span<lsm_tree_run_record_t const> runs) noexcept {
        return runs_.restore(channel, runs);
    }

#pragma endregion Writes

#pragma region Maintenance

    /**
     *  @brief Merges F memory runs of one tier into one run of the next tier, F being the level enlarge factor, until
     *      no tier holds F runs, as @c blobs::nvme_store does.
     *  @note The owner serializes it with flushes, compactions and truncations, and the calling thread is inside no
     *      epoch.
     */
    status_t merge_memory_runs() noexcept { return runs_.merge_memory_runs(fold_); }

    /** Whether the memory runs hold the bounds they were mounted with, so a flush is due. */
    [[nodiscard]] bool flush_due() const noexcept { return runs_.flush_due(); }

    /** Whether the memory runs are due for a merge, holding F runs of tier zero, or for a flush. */
    [[nodiscard]] bool maintenance_due() const noexcept { return runs_.maintenance_due(); }

    /**
     *  @brief Writes the memory runs of the published view as one run of level zero, heads in its cells and tails in
     *      its tails, with no lock held, then publishes a view naming that run instead of exactly those runs.
     *  @note The owner serializes flushes, compactions and truncations, and the calling thread is inside no epoch.
     */
    status_t flush(io_channel_t& channel) noexcept {
        return runs_.flush([&](tree_t& tree, std::span<memory_run_t const* const> flushed,
                               compaction_stamps_t const& survival,
                               lsm_tree_publication_hook_t const& published) noexcept -> status_t {
            stamped_deltas_t versions;
            utilities::local_arena_t folded;
            utilities::arena_allocator<byte_t> folded_bytes = folded.allocator();
            if (status_t const kept = kept_versions(flushed, survival, fold_, versions, folded_bytes); kept.failed())
                return kept;
            drained_t drained;
            if (status_t const laid = drain_(std::span<stamped_delta_t const> {versions}, drained); laid.failed())
                return laid;
            key_tails_column_t const tails = drained.tails.empty()
                                                 ? key_tails_column_t {}
                                                 : key_tails_column_t {
                                                       std::span<std::uint32_t const> {drained.tail_offsets},
                                                       std::span<byte_t const> {drained.tails}};
            auto [tombstones, tombstones_status] = gathered_range_tombstones(flushed, st::default_allocator<byte_t> {});
            if (tombstones_status.failed()) return tombstones_status;
            return tree.flush(channel, std::span<key_slot_t const> {drained.key_slots},
                              std::span<std::uint64_t const> {drained.stamps},
                              std::span<std::uint8_t const> {drained.kinds}, survival.floor, {}, {}, key_slot_t::size,
                              published, tails, tombstones);
        });
    }

    /** What an ingest keeps between its writes: the cells and tails laid out for the run it fills, and the runs. */
    struct ingest_runs_t;

    /** What an ingest into this collection starts from. */
    [[nodiscard]] ingest_runs_t begin_ingest() noexcept { return {}; }

    /**
     *  @brief Lays @p rows out behind the strings @p ingested laid out before, at stamp zero, heads in their cells and
     *      tails in their tails, as a flush lays them, writing a run each time one fills.
     *  @param[in] rows Strings in ascending order, each one 16-byte value under its string, readable until the call
     *      returns.
     *  @return Success, or the status that stopped it with @p ingested as it was.
     *
     *  A run is cut once its keys and layers would outgrow one slot, between any two strings, which
     *  an ingest takes distinct.
     */
    status_t write_ingest(io_channel_t& channel, std::span<keyed_delta_t const> rows,
                          ingest_runs_t& ingested) noexcept {
        if (rows.empty()) return {};
        drained_t& drained = ingested.drained;
        bool const continues = !ingested.runs.empty() || !drained.key_slots.empty();
        std::uint64_t tails = 0;
        for (std::size_t index = 0; index != rows.size(); ++index) {
            std::string_view const text = rows[index].stored_key;
            std::string_view const previous = index ? rows[index - 1].stored_key
                                                    : std::string_view {ingested.last.begin(), ingested.last.end()};
            if ((index || continues) && !(previous < text))
                return {status_code_t::invalid_argument_k, "An ingest takes its strings strictly ascending"};
            delta_view_t const& delta = rows[index].delta;
            if (delta.kind != delta_kind_t::write_k || !admits(delta.bytes.size()))
                return {status_code_t::malformed_k, "A string maps onto one 16-byte key"};
            tails += tail_of(text).size();
        }
        // The strings laid out before take a run of their own when these overflow it.
        bool const overflows = ingested_run_overflows_(drained.key_slots.size() + rows.size(),
                                                       drained.tails.size() + tails);
        if (overflows && !drained.key_slots.empty())
            if (status_t const written = write_ingested_run_(channel, drained, ingested.runs); written.failed())
                return written;
        std::size_t const runs_before = ingested.runs.size();
        std::size_t const cells_before = drained.key_slots.size();
        std::size_t const tails_before = drained.tails.size();
        if (status_t const laid = lay_out_ingested_(channel, rows, ingested); laid.failed()) {
            while (ingested.runs.size() != runs_before) {
                runs_.tree().volume().deallocate(ingested.runs.back().extent());
                ingested.runs.pop_back();
            }
            drained.resize(cells_before, tails_before);
            return laid;
        }
        std::string_view const kept = rows.back().stored_key;
        if (ingested.last.resize(kept.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "An ingest could not hold its last string"};
        if (!kept.empty()) std::memcpy(ingested.last.data(), kept.data(), kept.size());
        return {};
    }

    /** Writes the strings @p ingested laid out last as its final run, and publishes every run as one commit placed
     *  above every level holding their keys, flushing the memory runs first, as the blobs store does. */
    expected<transaction_stamp_t> publish_ingest(io_channel_t& channel, ingest_runs_t& ingested) noexcept {
        if (!ingested.drained.key_slots.empty())
            if (status_t const written = write_ingested_run_(channel, ingested.drained, ingested.runs);
                written.failed()) {
                drop_ingest(ingested);
                return written;
            }
        return runs_.publish_ingest(ingested.runs, [&]() noexcept { return flush(channel); });
    }

    /** Gives back every run @p ingested wrote and drops every string it laid out, which an ingest
     *  that never published leaves. */
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
     *  @param[in] compactor The compactor the database picked at open, which the tree passes over for runs carrying
     *      tails, as no device compaction is compiled for them.
     *  @param[in] published What to call once the merged set is live and before a consumed run is dropped.
     *  @param[in] fan_out The threads a merge fans its slices out over, which a merge of strings
     *      runs as one.
     */
    expected<compaction_outcome_t> compact(
        io_channel_t& reading_channel, io_channel_t& writing_channel, blobs::compactor_ref_t const& compactor,
        lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {},
        blobs::compaction_fan_out_t const& fan_out = blobs::compaction_fan_out_t {}) noexcept {
        return runs_.compact(reading_channel, writing_channel, compactor, fold_, blobs::no_device_fold_t {}, published,
                             fan_out);
    }

    /** Drops every version a mount found stamped above @p ceiling, which only a store holding no memory run takes. */
    status_t truncate_above(io_channel_t& reading_channel, io_channel_t& writing_channel, transaction_stamp_t ceiling,
                            lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {}) noexcept {
        return runs_.truncate_above(reading_channel, writing_channel, ceiling, fold_, published);
    }

#pragma endregion Maintenance

  private:

#pragma region Private Types

    /** The versions one merge or one flush carries over, before and after they are sorted into a run's order. */
    using stamped_deltas_t = st::vector<stamped_delta_t>;

    /** The deltas one batch of its own hands over. */
    using deltas_t = st::vector<keyed_delta_t, constrained_allocator<keyed_delta_t, atomic_reference_>>;

    /** The newest version of one string a read found: what it does, and its bytes in a memory run or its position in a
     *  run of a level. */
    struct found_t {

        /** What the version does, and its bytes when a memory run holds it. */
        delta_view_t delta;

        /** The run of a level holding it, or null for a memory run. */
        sorted_run_t const* run = nullptr;

        /** Its sorted position inside @c run. */
        std::uint64_t position = 0;

        /** The level holding @c run. */
        std::uint32_t level_number = 0;

        /** Where @c run sits inside that level. */
        std::uint32_t run_index = 0;

        /** The stamp of the version. */
        transaction_stamp_t stamp = unwritten_stamp_k;
    };

    /** Where one run a walk merges has got to, and where its window ends. */
    struct cursor_t {

        /** The memory run, or null for a run of a level. */
        memory_run_t const* memory = nullptr;

        /** The run of a level, or null for a memory run. */
        sorted_run_t const* level = nullptr;

        /** The level holding @c level. */
        std::uint32_t level_number = 0;

        /** Where @c level sits inside that level. */
        std::uint32_t run_index = 0;

        /** The next sorted position inside the run. */
        std::uint64_t position = 0;

        /** The first position past the window, below which a descending walk is yet to visit. */
        std::uint64_t end = 0;

        /** The first sorted position of the window, which a descending walk stops at. */
        std::uint64_t begin = 0;
    };

    /** The columns one flush writes: cells holding the values in ascending order, their stamps, kinds, and tails. */
    struct drained_t {
        st::vector<key_slot_t> key_slots;
        st::vector<std::uint64_t> stamps;
        st::vector<std::uint8_t> kinds;
        st::vector<std::uint32_t> tail_offsets;
        st::vector<byte_t> tails;

        /** Empties every column for the next run, keeping what they hold room for. */
        void clear() noexcept { key_slots.clear(), stamps.clear(), kinds.clear(), tail_offsets.clear(), tails.clear(); }

        /** Trims the columns back to @p cells cells and @p tail_bytes bytes of tails, as they stood before a layout. */
        void resize(std::size_t cells, std::size_t tail_bytes) noexcept {
            [[maybe_unused]] st::status_t trimmed = key_slots.resize(cells);
            trimmed = stamps.resize(cells);
            trimmed = kinds.resize(cells);
            trimmed = tail_offsets.resize(cells);
            trimmed = tails.resize(tail_bytes);
        }
    };

  public:
    struct ingest_runs_t {

        /** The cells, stamps, kinds and tails laid out for the run being filled. */
        drained_t drained;

        /** The runs written so far, in ascending order of their strings. */
        st::vector<sorted_run_t> runs;

        /** The last string laid out, which the next one must ascend past. */
        st::vector<char> last;
    };

  private:

#pragma endregion Private Types

#pragma region Private Methods

    /** The head of the key at sorted @p position of a memory run. */
    [[nodiscard]] static st::key128_t head_at_(memory_run_t const& run, std::uint64_t position) noexcept {
        return run.head(static_cast<std::size_t>(position));
    }

    /** The tail of the key at sorted @p position of a memory run. */
    [[nodiscard]] static std::string_view tail_at_(memory_run_t const& run, std::uint64_t position) noexcept {
        return tail_of(run.stored_key(static_cast<std::size_t>(position)));
    }

    /** The head of the key at sorted @p position of a run of a level. */
    [[nodiscard]] static st::key128_t head_at_(sorted_run_t const& run, std::uint64_t position) noexcept {
        return run.head(position);
    }

    /** The tail of the key at sorted @p position of a run of a level. */
    [[nodiscard]] static std::string_view tail_at_(sorted_run_t const& run, std::uint64_t position) noexcept {
        return tail_of(run.key(position));
    }

    /**
     *  @brief The first sorted position of @p run past every string starting with @p prefix: the
     *      lower bound of the least string above all of them, or the run's end when no string is.
     *  @return The position, or @c out_of_memory_k when that string had no room.
     */
    static expected<std::uint64_t> prefix_end_in_(sorted_run_t const& run, std::string_view prefix) noexcept {
        std::size_t kept = prefix.size();
        while (kept != 0 && static_cast<unsigned char>(prefix[kept - 1]) == 0xFF) --kept;
        if (kept == 0) return run.population_count();
        st::vector<char> above;
        if (above.resize(kept) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room to bound a prefix"};
        std::memcpy(above.data(), prefix.data(), kept);
        above[kept - 1] = static_cast<char>(static_cast<unsigned char>(above[kept - 1]) + 1);
        return run.lower_bound(std::string_view {above.data(), kept});
    }

    /** The run of the sorted @p level whose keys reach @p text, whose head is @p head, which only
     *  one run's may, or null: runs order by heads, and by their first and last keys whole only
     *  where a head spans two of them. */
    [[nodiscard]] static sorted_run_t const* run_holding_(typename tree_t::level_t const& level, std::string_view text,
                                                          st::key128_t const& head) noexcept {
        std::size_t low = 0;
        std::size_t high = level.size();
        while (low != high) {
            std::size_t const middle = low + (high - low) / 2;
            sorted_run_t const& run = level.run(middle);
            std::uint64_t const count = run.population_count();
            // A run of tombstones alone holds no string; no other level run has one it hides.
            if (count == 0) {
                if (text < run.lower_string()) high = middle;
                else low = middle + 1;
            }
            else if (run.upper_stored_key() < head || (run.upper_stored_key() == head && run.key(count - 1) < text))
                low = middle + 1;
            else if (head < run.lower_stored_key() || (run.lower_stored_key() == head && text < run.key(0)))
                high = middle;
            else return &run;
        }
        return nullptr;
    }

    /** Whether @p run holds a version of @p text, whose head is @p head, stamped at or below @p at,
     *  answering the newest one in @p found: the heads and the layers narrow it to one row of keys,
     *  compared whole in order. */
    [[nodiscard]] static bool version_in_(sorted_run_t const& run, std::string_view text, st::key128_t const& head,
                                          transaction_stamp_t at, found_t& found) noexcept {
        if (run.population_count() == 0 || head < run.lower_stored_key() || run.upper_stored_key() < head) return false;
        key_positions_t const range = run.key_range(text);
        for (std::uint64_t position = range.first; position != range.last; ++position)
            if (run.stamp(position) <= at && run.key(position) == text) {
                found = found_t {delta_view_t {run.kind(position), {}}, &run, position, 0, 0, run.stamp(position)};
                return true;
            }
        return false;
    }

    /** Whether any level of @p levels holds a version of the string of @p head and @p tail at or below @p at,
     *  answering the newest in @p found: the youngest run of level zero first, then each sorted level's one run. */
    [[nodiscard]] static bool newest_in_levels_(level_set_t const& levels, std::string_view text,
                                                transaction_stamp_t at, found_t& found) noexcept {
        st::key128_t const head = head_of(text);
        for (std::size_t number = 0; number != levels.size(); ++number) {
            auto const& level = levels.level(number);
            if (level.order() == lsm_level_order_t::overlapping_k) {
                for (std::size_t index = level.size(); index-- != 0;)
                    if (version_in_(level.run(index), text, head, at, found)) {
                        found.level_number = static_cast<std::uint32_t>(number);
                        found.run_index = static_cast<std::uint32_t>(index);
                        return true;
                    }
                continue;
            }
            if (sorted_run_t const* const run = run_holding_(level, text, head))
                if (version_in_(*run, text, head, at, found)) {
                    found.level_number = static_cast<std::uint32_t>(number);
                    return true;
                }
        }
        return false;
    }

    /** Whether a range tombstone @p view shows a reader at @p at hides the version @p found of
     *  @p text: one of a newer memory run for a memory run's version, and for a level's one of a
     *  memory run, a shallower level, a younger run of level zero, or of its own run stamped above
     *  it, as merges raise version stamps and tombstones keep theirs. */
    [[nodiscard]] static bool hidden_(view_t const& view, std::string_view text, transaction_stamp_t at,
                                      found_t const& found) noexcept {
        transaction_stamp_t const in_memory = view.memory_runs.covering_stamp(text, at);
        if (!found.run) return in_memory > found.stamp;
        if (in_memory != unwritten_stamp_k) return true;
        level_set_t const& levels = *view.levels;
        for (std::size_t number = 0; number <= found.level_number; ++number) {
            auto const& level = levels.level(number);
            if (level.range_tombstones_count() == 0) continue;
            bool const shallower = number != found.level_number;
            for (std::size_t index = 0; index != level.size(); ++index) {
                sorted_run_t const& run = level.run(index);
                bool const own = !shallower && &run == found.run;
                bool const younger = !shallower && level.order() == lsm_level_order_t::overlapping_k &&
                                     index > found.run_index;
                if (!shallower && !younger && !own) continue;
                transaction_stamp_t const covering = run.range_tombstones().covering_stamp(text, at);
                if (covering == unwritten_stamp_k) continue;
                if (!own || covering > found.stamp) return true;
            }
        }
        return false;
    }

    /** Whether a tombstone of @p tombstones stamped after @p from hides a string starting with
     *  @p prefix and not below @p first. */
    template <typename tombstones_type_>
    [[nodiscard]] static bool prefix_removed_since_(tombstones_type_ const& tombstones, std::string_view prefix,
                                                    std::string_view first, transaction_stamp_t from) noexcept {
        for (std::size_t index = 0; index != tombstones.size(); ++index) {
            range_t const& range = tombstones.range(index);
            bool const reaches_prefix = range.start < prefix || range.start.starts_with(prefix);
            bool const reaches_first = range.end.empty() || first < range.end;
            if (reaches_prefix && reaches_first && tombstones.stamp(index) > from) return true;
        }
        return false;
    }

    /** Whether any memory run or level of @p view holds a version of @p text at or below @p at, answering the newest in
     *  @p found. */
    [[nodiscard]] static bool newest_(view_t const& view, std::string_view text, transaction_stamp_t at,
                                      found_t& found) noexcept {
        if (view.memory_runs.find(text, at, found.delta, found.stamp)) return true;
        return newest_in_levels_(*view.levels, text, at, found);
    }

    /**
     *  @brief Lists into @p page the string of @p head and @p tail with what its newest version at
     *      or below @p stamp carries, moving every cursor of @p cursors sitting on it past its
     *      versions, and lists nothing when no version is old enough.
     *
     *  Cursors sit newest first with disjoint stamps, so the first version at or below the stamp is
     *  the newest; a range tombstone hiding it lists a removal.
     */
    static status_t list_key_(std::span<cursor_t> cursors, st::key128_t const& head, std::string_view tail,
                              transaction_stamp_t stamp, view_t const& view, strings_page_t& page) noexcept {
        listed_string_t listed;
        found_t version_found;
        bool taken = false;
        // Cursors sit newest first with disjoint stamps, so the first version within the stamp wins
        for (cursor_t& cursor : cursors)
            for (; cursor.position != cursor.end &&
                   order_keys(cursor_head_(cursor), cursor_tail_(cursor), head, tail) == 0;
                 ++cursor.position) {
                if (taken) continue;
                transaction_stamp_t const version = cursor.memory ? cursor.memory->stamp(cursor.position)
                                                                  : cursor.level->stamp(cursor.position);
                if (version > stamp) continue;
                taken = true;
                version_found = found_t {{},     cursor.level, cursor.position, cursor.level_number, cursor.run_index,
                                         version};
                delta_kind_t const kind = cursor.memory ? cursor.memory->delta(cursor.position).kind
                                                        : cursor.level->kind(cursor.position);
                if (kind == delta_kind_t::remove_k) {
                    listed.state = lookup_result_t::removed_k;
                    continue;
                }
                listed.state = lookup_result_t::written_k;
                std::memcpy(&listed.value,
                            cursor.memory ? cursor.memory->delta(cursor.position).bytes.data()
                                          : cursor.level->value(cursor.position).data(),
                            key_slot_t::size);
            }
        if (!taken) return {};

        std::string_view text;
        if (status_t const joined = string_of_key(head, tail, page.whole, text); joined.failed()) return joined;
        if (listed.state == lookup_result_t::written_k && hidden_(view, text, stamp, version_found))
            listed.state = lookup_result_t::removed_k;
        listed.offset = page.texts.size();
        listed.length = text.size();
        if (page.texts.resize(listed.offset + listed.length) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A walk could not hold its page"};
        if (!text.empty()) std::memcpy(page.texts.data() + listed.offset, text.data(), text.size());
        if (page.listed.push_back(std::move(listed)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A walk could not hold its page"};
        return {};
    }

    /** Whether @p check answers true for any run of any level of @p levels. */
    template <typename check_type_>
    [[nodiscard]] static bool any_level_run_(level_set_t const& levels, check_type_&& check) noexcept {
        for (std::size_t number = 0; number != levels.size(); ++number)
            for (std::size_t index = 0; index != levels.level(number).size(); ++index)
                if (check(levels.level(number).run(index))) return true;
        return false;
    }

    /** The head and the tail of the key a walk's @p cursor sits on. */
    [[nodiscard]] static st::key128_t cursor_head_(cursor_t const& cursor) noexcept {
        return cursor.memory ? head_at_(*cursor.memory, cursor.position) : head_at_(*cursor.level, cursor.position);
    }

    [[nodiscard]] static std::string_view cursor_tail_(cursor_t const& cursor) noexcept {
        return cursor.memory ? tail_at_(*cursor.memory, cursor.position) : tail_at_(*cursor.level, cursor.position);
    }

    /** Lays @p versions out as one run's cells, each holding its value, and their stamps, kinds and tails. */
    status_t drain_(std::span<stamped_delta_t const> versions, drained_t& drained) const noexcept {
        std::uint64_t tails_bytes = 0;
        for (stamped_delta_t const& version : versions) tails_bytes += tail_of(version.stored_key).size();
        if (drained.key_slots.resize(versions.size()) != st::status_t::success_k ||
            drained.stamps.resize(versions.size()) != st::status_t::success_k ||
            drained.kinds.resize(versions.size()) != st::status_t::success_k ||
            drained.tail_offsets.resize(tails_bytes != 0 ? versions.size() + 1 : 0) != st::status_t::success_k ||
            drained.tails.resize(static_cast<std::size_t>(tails_bytes)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A flush could not hold its columns"};

        std::uint64_t written_tails = 0;
        for (std::size_t index = 0; index != versions.size(); ++index) {
            stamped_delta_t const& version = versions[index];
            std::span<byte_t const> const bytes = version.delta.bytes;
            std::string_view const tail = tail_of(version.stored_key);
            key_slot_t& built = drained.key_slots[index];
            built = key_slot_t {};
            built.stored_key = head_of(version.stored_key);
            if (!bytes.empty()) std::memcpy(built.data, bytes.data(), key_slot_t::size);
            if (tails_bytes != 0) drained.tail_offsets[index] = static_cast<std::uint32_t>(written_tails);
            if (!tail.empty()) std::memcpy(drained.tails.data() + written_tails, tail.data(), tail.size());
            drained.stamps[index] = version.stamp;
            drained.kinds[index] = static_cast<std::uint8_t>(version.delta.kind);
            written_tails += tail.size();
        }
        if (tails_bytes != 0) drained.tail_offsets[versions.size()] = static_cast<std::uint32_t>(written_tails);
        return {};
    }

    /** Whether a run of @p key_slots strings carrying @p tails bytes of tails outgrows one slot:
     *  its keys whole, its trailer of zero-width stamps, its kinds at a byte each, and layers at
     *  most a head and a row's share per 15 bytes of every key. */
    [[nodiscard]] static bool ingested_run_overflows_(std::uint64_t key_slots, std::uint64_t tails) noexcept {
        using strings_layout_t = typename sorted_run_t::strings_layout_t;
        std::uint64_t const keys = tails + key_slots * head_prefix_bytes_k;
        std::uint64_t const layers = 2 * sizeof(st::key128_t) * (tails / head_prefix_bytes_k + key_slots);
        std::uint64_t const bytes = strings_layout_t::blob_bytes(key_slots, keys, utilities::portable_alignment_k,
                                                                 key_slots + utilities::portable_alignment_k, layers);
        return bytes > utilities::sorted_run_bytes_max_k;
    }

    /** Writes the cells, stamps, kinds and tails laid out so far as one run of stamp zero into @p runs, and empties
     *  them for the next. */
    status_t write_ingested_run_(io_channel_t& channel, drained_t& drained, st::vector<sorted_run_t>& runs) noexcept {
        if (runs.reserve(runs.size() + 1) != st::status_t::success_k ||
            drained.tail_offsets.push_back(static_cast<std::uint32_t>(drained.tails.size())) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "An ingest could not hold another run"};
        key_tails_column_t const tails = drained.tails.empty()
                                             ? key_tails_column_t {}
                                             : key_tails_column_t {
                                                   std::span<std::uint32_t const> {drained.tail_offsets},
                                                   std::span<byte_t const> {drained.tails}};
        auto [written, write_status] = sorted_run_t::write(
            runs_.tree().volume(), channel, runs_.config().lsm_tree.collection, utilities::uuid_generator(),
            std::span<key_slot_t const> {drained.key_slots}, std::span<std::uint64_t const> {drained.stamps},
            std::span<std::uint8_t const> {drained.kinds}, 0, {}, {}, key_slot_t::size, tails);
        if (write_status.failed()) {
            drained.tail_offsets.pop_back();
            return write_status;
        }
        [[maybe_unused]] st::status_t const placed = runs.push_back(st::assume_reserved, std::move(written));
        drained.clear();
        return {};
    }

    /** Lays @p rows out behind what @p ingested laid out, writing a run each time one fills. */
    status_t lay_out_ingested_(io_channel_t& channel, std::span<keyed_delta_t const> rows,
                               ingest_runs_t& ingested) noexcept {
        drained_t& drained = ingested.drained;
        for (keyed_delta_t const& row : rows) {
            st::key128_t const head = head_of(row.stored_key);
            std::string_view const tail = tail_of(row.stored_key);
            bool const full = ingested_run_overflows_(drained.key_slots.size() + 1, drained.tails.size() + tail.size());
            if (full && !drained.key_slots.empty())
                if (status_t const written = write_ingested_run_(channel, drained, ingested.runs); written.failed())
                    return written;

            key_slot_t built {};
            built.stored_key = head;
            std::memcpy(built.data, row.delta.bytes.data(), key_slot_t::size);
            std::size_t const tails_before = drained.tails.size();
            if (drained.key_slots.push_back(std::move(built)) != st::status_t::success_k ||
                drained.stamps.push_back(std::uint64_t {0}) != st::status_t::success_k ||
                drained.kinds.push_back(static_cast<std::uint8_t>(delta_kind_t::write_k)) != st::status_t::success_k ||
                drained.tail_offsets.push_back(static_cast<std::uint32_t>(tails_before)) != st::status_t::success_k ||
                drained.tails.resize(tails_before + tail.size()) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "An ingest could not hold its strings"};
            if (!tail.empty()) std::memcpy(drained.tails.data() + tails_before, tail.data(), tail.size());
        }
        return {};
    }

    /** Room for @p count deltas of one batch of its own. */
    expected<deltas_t> deltas_of_(std::size_t count) const noexcept {
        deltas_t deltas {constrained_allocator<keyed_delta_t, atomic_reference_> {runs_.constraint()}};
        if (deltas.resize(count) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room to hand a batch over"};
        return deltas;
    }

    /** Seals the last delta of every string of @p deltas, which a batch naming a string twice takes. */
    expected<sealed_commit_t> seal_last_of_each_(deltas_t& deltas) const noexcept {
        auto [kept, sort_status] = sort_unique_strings(std::span<keyed_delta_t> {deltas}, strings_kit_type_::sort_order,
                                                       keep_t::last_k, deltas.get_allocator());
        if (sort_status.failed()) return sort_status;
        return seal(std::span<keyed_delta_t> {deltas.data(), kept});
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The tree, memory runs and view this store publishes, and every merge, flush and compaction of them. */
    published_runs_t runs_;

    /** What a delta does, which merges fold with. */
    [[no_unique_address]] fold_t fold_ {};

#pragma endregion Private Members
};

#pragma endregion Store

#pragma region Transaction

/**
 *  @brief The strings one database transaction writes and watches, keyed by whole strings, which the transaction lays
 *      out and lands with its own writes through the share this joins it with.
 *  @tparam database_type_ The database every collection lives in, a @c blobs::basic_database.
 *  @tparam store_type_ The store its strings collections live in.
 *
 *  A string written twice keeps its last write and its earliest watch. A read is watched apart from the writes, and a
 *  walk as a range of strings, so a commit landing on any watched string since its watch conflicts, the strings no
 *  walk found included, and two strings sharing a head never conflict with each other. The strings sit on one tape
 *  this owns, which the tables locate them on, and each write keeps its 16-byte value beside it, so nothing here moves
 *  once constructed, and nothing is shared: a transaction belongs to the thread holding it.
 */
template <typename database_type_, typename store_type_>
class nvme_transaction {
  public:

    /** The database every collection lives in. */
    using database_t = database_type_;

    /** The store every strings collection of the database lives in. */
    using store_t = store_type_;

    /** The database transaction this joins. */
    using transaction_t = typename database_t::transaction_t;

    /** One delta a commit hands a store over. */
    using keyed_delta_t = keyed_delta<std::string_view>;

    /** The strings one range removal hides. */
    using range_t = range_tombstone<std::string_view>;

    /** A transaction of @p database writing and watching nothing yet, joined to none. */
    explicit nvme_transaction(database_t& database) noexcept
        : database_(&database), writes_(place_hash_t {&tape_}, place_equals_t {&tape_}) {
        share_.context = this;
        share_.prepare = &prepare_share_;
        share_.publish = &publish_share_;
        share_.forget = &forget_share_;
    }

    nvme_transaction(nvme_transaction const&) = delete;
    nvme_transaction(nvme_transaction&&) = delete;
    nvme_transaction& operator=(nvme_transaction const&) = delete;
    nvme_transaction& operator=(nvme_transaction&&) = delete;

    /** Joins @p transaction, which lays these strings out and lands them with every stage and commit from now on, and
     *  which this must outlive. */
    void join(transaction_t& transaction) noexcept { transaction.join(share_); }

    /** Writes @p delta, a 16-byte value or a removal, under @p text of @p collection, watched from @p watch_stamp,
     *  which only lowers the watch an earlier write of the string took, or @c invalid_transaction_stamp_k for none. */
    status_t write(collection_id_t collection, std::string_view text, delta_view_t delta,
                   transaction_stamp_t watch_stamp) noexcept {
        std::size_t const carried = delta.kind == delta_kind_t::remove_k ? 0 : sizeof(ustore_key_t);
        if (delta.bytes.size() != carried)
            return {status_code_t::invalid_argument_k, "A string maps onto one 16-byte key"};
        if (writes_.reserve_more(1) == writes_t::reserve_result_t::failed_k)
            return {status_code_t::out_of_memory_k, "No room for another string"};
        write_t written {ustore_key_t {}, delta.kind, watch_stamp};
        if (carried != 0) std::memcpy(&written.value, delta.bytes.data(), carried);
        bool updated = false;
        writes_.probe_to_find(probe_t {collection, text}, [&](auto& slot) noexcept {
            write_t& held = slot.value_ref();
            transaction_stamp_t const earlier = std::min(held.watch_stamp, watch_stamp);
            held = written;
            held.watch_stamp = earlier;
            updated = true;
        });
        if (updated) return {};
        auto [place, place_status] = append_(text);
        if (place_status.failed()) return place_status;
        place.collection = collection;
        writes_.emplace(place, written, st::assume_reserved);
        return {};
    }

    /** Removes every string of @p collection from @p start up to @p end, exclusive, or to the
     *  last string for an empty @p end, as one range landing with the commit, dropping every
     *  string this transaction wrote inside it, as the range hides only older versions; a later
     *  write survives it. */
    status_t remove_range(collection_id_t collection, std::string_view start, std::string_view end) noexcept {
        if (!end.empty() && !(start < end))
            return {status_code_t::invalid_argument_k, "A range ends at or before its start"};
        range_t const range {start, end};
        st::vector<std::string_view> inside;
        status_t listed;
        [[maybe_unused]] st::status_t const walked = writes_.for_each([&](auto const& slot) noexcept {
            if (listed.succeeded() && slot.key_ref().collection == collection && range.covers(text_of_(slot.key_ref())))
                listed = growth_status_(inside.push_back(text_of_(slot.key_ref())));
        });
        if (listed.failed()) return listed;
        for (std::string_view const text : inside) [[maybe_unused]]
            bool const erased = writes_.erase(probe_t {collection, text});
        auto [first, first_status] = append_(start);
        if (first_status.failed()) return first_status;
        auto [last, last_status] = append_(end);
        if (last_status.failed()) return last_status;
        first.collection = collection;
        return growth_status_(removed_ranges_.push_back(range_watch_t {first, last, invalid_transaction_stamp_k}));
    }

    /** Hands @p callback every range this transaction removes from @p collection, in the order
     *  removed, as @c callback(range), viewing this transaction until the next write. */
    template <typename callback_type_>
    void for_each_removed_range(collection_id_t collection, callback_type_&& callback) const noexcept {
        for (range_watch_t const& removed : removed_ranges_)
            if (removed.first.collection == collection)
                callback(range_t {text_of_(removed.first), text_of_(removed.second)});
    }

    /** What this transaction wrote under @p text of @p collection, viewing its value until
     *  the next write, a removal for a string inside a range it removed, and whether it
     *  did anything there. */
    [[nodiscard]] bool uncommitted(collection_id_t collection, std::string_view text,
                                   delta_view_t& found) const noexcept {
        bool written = false;
        writes_.probe_to_find(probe_t {collection, text}, [&](auto const& slot) noexcept {
            found = delta_of_(slot.value_ref());
            written = true;
        });
        if (written) return true;
        for_each_removed_range(collection, [&](range_t const& range) noexcept {
            if (range.covers(text)) written = true;
        });
        if (written) found = delta_view_t {delta_kind_t::remove_k, {}};
        return written;
    }

    /** Hands @p callback every string this transaction wrote into @p collection, in no order, as
     *  @c callback(text, delta), both viewing this transaction until the next write. */
    template <typename callback_type_>
    void for_each_uncommitted(collection_id_t collection, callback_type_&& callback) const noexcept {
        [[maybe_unused]] st::status_t const walked = writes_.for_each([&](auto const& slot) noexcept {
            if (slot.key_ref().collection == collection)
                callback(text_of_(slot.key_ref()), delta_of_(slot.value_ref()));
        });
    }

    /** Watches @p text of @p collection from @p watch_stamp, which the transaction read. */
    status_t watch(collection_id_t collection, std::string_view text, transaction_stamp_t watch_stamp) noexcept {
        auto [place, place_status] = append_(text);
        if (place_status.failed()) return place_status;
        place.collection = collection;
        return growth_status_(reads_.push_back(read_watch_t {place, watch_stamp}));
    }

    /** Watches every string of @p collection from @p lower to @p upper inclusive from @p watch_stamp, which a walk
     *  listed or passed. */
    status_t watch_range(collection_id_t collection, std::string_view lower, std::string_view upper,
                         transaction_stamp_t watch_stamp) noexcept {
        auto [first, first_status] = append_(lower);
        if (first_status.failed()) return first_status;
        auto [last, last_status] = append_(upper);
        if (last_status.failed()) return last_status;
        first.collection = collection;
        return growth_status_(ranges_.push_back(range_watch_t {first, last, watch_stamp}));
    }

    /** Watches every string of @p collection starting with @p prefix and not below @p lower from @p watch_stamp, which
     *  a walk that reached the end of the prefix passed. */
    status_t watch_prefix(collection_id_t collection, std::string_view prefix, std::string_view lower,
                          transaction_stamp_t watch_stamp) noexcept {
        auto [prefixed, prefixed_status] = append_(prefix);
        if (prefixed_status.failed()) return prefixed_status;
        auto [first, first_status] = append_(lower);
        if (first_status.failed()) return first_status;
        prefixed.collection = collection;
        return growth_status_(prefixes_.push_back(range_watch_t {prefixed, first, watch_stamp}));
    }

    /** Seals the strings, then stages @p transaction, which this joined, holding the ledger's leader word until the
     *  transaction commits or lets it go. */
    status_t stage(transaction_t& transaction) noexcept {
        if (status_t const sealed = seal_(); sealed.failed()) return sealed;
        return transaction.stage();
    }

    /** Seals the strings, then commits @p transaction, which this joined, and maintains every strings collection it
     *  wrote once it landed; a refused commit keeps every write and watch. */
    status_t commit(transaction_t& transaction) noexcept {
        if (status_t const sealed = seal_(); sealed.failed()) return sealed;
        if (status_t const committed = transaction.commit(); committed.failed()) {
            sealed_.clear();
            return committed;
        }
        // The commit has landed, so a failure to maintain is met again by the collection's next commit.
        for (sealed_collection_t const& sealed : sealed_) [[maybe_unused]]
            status_t const maintained = database_->maintain(sealed.collection);
        clear();
        return {};
    }

    /** Drops every write and watch, which a commit that landed and a rollback both do. */
    void clear() noexcept {
        writes_.clear();
        reads_.clear();
        ranges_.clear();
        prefixes_.clear();
        removed_ranges_.clear();
        sealed_.clear();
        tape_.clear();
    }

  private:

    /** Where one string sits on the tape, and the collection it belongs to. */
    struct place_t {
        collection_id_t collection {};
        std::uint64_t offset = 0;
        std::uint32_t size = 0;
    };

    /** A string of a collection to find, held by the caller. */
    struct probe_t {
        collection_id_t collection {};
        std::string_view text;
    };

    /** What a transaction wrote under one string: the 16 bytes of its value, zero for a removal, what it does, and
     *  its watch. */
    struct write_t {
        ustore_key_t value {};
        delta_kind_t kind = delta_kind_t::write_k;
        transaction_stamp_t watch_stamp = invalid_transaction_stamp_k;
    };

    /** One string read, or one range of strings walked, and the stamp a commit after which conflicts. */
    struct read_watch_t {
        place_t text;
        transaction_stamp_t watch_stamp = invalid_transaction_stamp_k;
    };

    struct range_watch_t {
        place_t first;
        place_t second;
        transaction_stamp_t watch_stamp = invalid_transaction_stamp_k;
    };

    /** One written collection's strings, sealed by the committer and landed by the leader. */
    struct sealed_collection_t {
        collection_id_t collection {};
        typename store_t::sealed_commit_t commit;
        blobs::memory_run_claim_t record;
    };

    /** Hashes a string of a collection through StringZilla, the same whether a tape or a caller holds it. */
    struct place_hash_t {
        st::vector<char> const* tape = nullptr;

        [[nodiscard]] std::size_t hash_(collection_id_t const& collection, std::string_view text) const noexcept {
            // StringZilla takes no null pointer, which an empty string may view.
            sz_u64_t hashed = 0;
            if (!text.empty()) store_t::strings_kit_t::hash(text.data(), text.size(), 0, &hashed, nullptr);
            return static_cast<std::size_t>(hashed) ^ (collection_id_hash_t {}(collection) * 0x9E3779B97F4A7C15ull);
        }
        std::size_t operator()(place_t const& place) const noexcept {
            return hash_(place.collection, std::string_view {tape->data() + place.offset, place.size});
        }
        std::size_t operator()(probe_t const& probe) const noexcept { return hash_(probe.collection, probe.text); }
    };

    /** Tells two strings of a collection apart, whether a tape or a caller holds them. */
    struct place_equals_t {
        st::vector<char> const* tape = nullptr;

        [[nodiscard]] std::string_view text_(place_t const& place) const noexcept {
            return std::string_view {tape->data() + place.offset, place.size};
        }
        bool operator()(place_t const& first, place_t const& second) const noexcept {
            return first.collection == second.collection && text_(first) == text_(second);
        }
        bool operator()(place_t const& first, probe_t const& second) const noexcept {
            return first.collection == second.collection && text_(first) == second.text;
        }
        bool operator()(probe_t const& first, place_t const& second) const noexcept { return (*this)(second, first); }
    };

    using writes_t = st::hash_map<place_t, write_t, place_hash_t, place_equals_t>;

    static status_t growth_status_(st::status_t status) noexcept {
        if (st::succeeded(status)) return {};
        return {status_code_t::out_of_memory_k, "No room to watch another string"};
    }

    /** Copies @p bytes onto the tape, answering where they sit, for a collection the caller fills in. */
    expected<place_t> append_(std::string_view bytes) noexcept {
        std::size_t const offset = tape_.size();
        if (tape_.resize(offset + bytes.size()) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room for another string"};
        if (!bytes.empty()) std::memcpy(tape_.data() + offset, bytes.data(), bytes.size());
        place_t place;
        place.offset = offset;
        place.size = static_cast<std::uint32_t>(bytes.size());
        return place;
    }

    [[nodiscard]] std::string_view text_of_(place_t const& place) const noexcept {
        return std::string_view {tape_.data() + place.offset, place.size};
    }

    [[nodiscard]] static delta_view_t delta_of_(write_t const& written) noexcept {
        if (written.kind == delta_kind_t::remove_k) return delta_view_t {written.kind, {}};
        return delta_view_t {written.kind, std::as_bytes(std::span<ustore_key_t const, 1> {&written.value, 1})};
    }

    /** Calls @p visitor with the strings store of @p collection inside the database's epochs, answering what it
     *  answers, or @c write_conflict_k for a collection gone since or kept by another store. */
    template <typename visitor_type_>
    status_t with_store_(collection_id_t collection, visitor_type_&& visitor) noexcept {
        status_t const visited = database_->visit_store(
            collection, [&]<typename held_type_>(held_type_& store) noexcept -> status_t {
                if constexpr (std::is_same_v<held_type_, store_t>) return visitor(store);
                else return {status_code_t::write_conflict_k, "A strings collection the transaction wrote is gone"};
            });
        if (visited == status_code_t::not_found_k)
            return {status_code_t::write_conflict_k, "A strings collection the transaction wrote is gone"};
        return visited;
    }

    /** Seals every written collection's strings on the committing thread, making room first when a collection's
     *  memory runs are past twice their bounds. */
    status_t seal_() noexcept {
        sealed_.clear();
        st::vector<keyed_delta_t> deltas;
        st::vector<collection_id_t> collections;
        status_t listed;
        [[maybe_unused]] st::status_t const walked = writes_.for_each([&](auto const& slot) noexcept {
            collection_id_t const collection = slot.key_ref().collection;
            if (listed.failed()) return;
            for (collection_id_t const& seen : collections)
                if (seen == collection) return;
            listed = growth_status_(collections.push_back(collection_id_t {collection}));
        });
        for (range_watch_t const& removed : removed_ranges_) {
            bool seen = false;
            for (collection_id_t const& held : collections) seen |= held == removed.first.collection;
            if (!seen && listed.succeeded())
                listed = growth_status_(collections.push_back(collection_id_t {removed.first.collection}));
        }
        if (listed.failed()) return listed;
        st::vector<range_t> ranges;

        for (collection_id_t const& collection : collections) {
            deltas.clear();
            for_each_uncommitted(collection, [&](std::string_view text, delta_view_t delta) noexcept {
                if (listed.succeeded()) listed = growth_status_(deltas.push_back(keyed_delta_t {text, delta}));
            });
            ranges.clear();
            for_each_removed_range(collection, [&](range_t const& range) noexcept {
                if (listed.succeeded()) listed = growth_status_(ranges.push_back(range_t {range}));
            });
            if (listed.failed()) return listed;
            sealed_collection_t sealed;
            sealed.collection = collection;
            if (status_t const made = with_store_(collection,
                                                  [&](store_t& store) noexcept -> status_t {
                                                      auto [commit, seal_status] = store.seal(
                                                          std::span<keyed_delta_t> {deltas},
                                                          std::span<range_t const> {ranges});
                                                      if (seal_status.failed()) return seal_status;
                                                      sealed.commit = std::move(commit);
                                                      return {};
                                                  });
                made.failed())
                return made;
            if (status_t const room = database_->make_room(collection, sealed.commit); room.failed()) return room;
            // Claimed before the leader word, waiting on a flush when every record is out, so the
            // share cannot be refused one.
            if (!sealed.commit.empty())
                if (status_t const claimed = database_->claim_memory_record(collection, sealed.record);
                    claimed.failed())
                    return claimed;
            if (sealed_.push_back(std::move(sealed)) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "No room to seal a commit"};
        }
        return {};
    }

    /** Whether a commit since its watch wrote any string this transaction watches. */
    [[nodiscard]] bool conflicting_() noexcept {
        bool conflict = false;
        auto const check = [&](collection_id_t collection, auto&& written) noexcept {
            if (conflict) return;
            status_t const checked = with_store_(collection, [&](store_t const& store) noexcept -> status_t {
                conflict = written(store);
                return {};
            });
            if (checked.failed()) conflict = true;
        };
        [[maybe_unused]] st::status_t const walked = writes_.for_each([&](auto const& slot) noexcept {
            transaction_stamp_t const from = slot.value_ref().watch_stamp;
            if (from == invalid_transaction_stamp_k) return;
            std::string_view const text = text_of_(slot.key_ref());
            check(slot.key_ref().collection,
                  [&](store_t const& store) noexcept { return store.written_since(text, from); });
        });
        for (read_watch_t const& read : reads_)
            check(read.text.collection, [&](store_t const& store) noexcept {
                return store.written_since(text_of_(read.text), read.watch_stamp);
            });
        for (range_watch_t const& range : ranges_)
            check(range.first.collection, [&](store_t const& store) noexcept {
                return store.range_written_since(text_of_(range.first), text_of_(range.second), range.watch_stamp);
            });
        for (range_watch_t const& range : prefixes_)
            check(range.first.collection, [&](store_t const& store) noexcept {
                return store.prefix_written_since(text_of_(range.first), text_of_(range.second), range.watch_stamp);
            });
        return conflict;
    }

    /** Forgets the views the first @p prepared sealed collections laid out. */
    void forget_(std::size_t prepared) noexcept {
        for (std::size_t index = 0; index != prepared; ++index) [[maybe_unused]]
            status_t const forgotten = with_store_(sealed_[index].collection, [](store_t& store) noexcept -> status_t {
                store.forget_prepared();
                return {};
            });
    }

    static status_t prepare_share_(void* context) noexcept {
        nvme_transaction& owner = *static_cast<nvme_transaction*>(context);
        if (owner.conflicting_()) return {status_code_t::write_conflict_k, "Strings conflict"};
        for (std::size_t index = 0; index != owner.sealed_.size(); ++index) {
            sealed_collection_t& sealed = owner.sealed_[index];
            if (status_t const prepared = owner.with_store_(sealed.collection,
                                                            [&](store_t& store) noexcept -> status_t {
                                                                store.prepare(sealed.commit, std::move(sealed.record));
                                                                return {};
                                                            });
                prepared.failed()) {
                owner.forget_(index);
                return prepared;
            }
        }
        return {};
    }

    static void publish_share_(void* context, transaction_stamp_t stamp) noexcept {
        nvme_transaction& owner = *static_cast<nvme_transaction*>(context);
        for (sealed_collection_t& sealed : owner.sealed_) [[maybe_unused]]
            status_t const published = owner.with_store_(sealed.collection, [&](store_t& store) noexcept -> status_t {
                store.publish(sealed.commit, stamp);
                return {};
            });
    }

    static void forget_share_(void* context) noexcept {
        nvme_transaction& owner = *static_cast<nvme_transaction*>(context);
        owner.forget_(owner.sealed_.size());
    }

    /** The database every collection lives in, which outlives this. */
    database_t* database_ = nullptr;

    /** Every string this transaction wrote or watched, back to back. */
    st::vector<char> tape_;

    /** The last write of every string, located on the tape. */
    writes_t writes_;

    /** Every string read and watched. */
    st::vector<read_watch_t> reads_;

    /** Every closed range of strings a walk listed. */
    st::vector<range_watch_t> ranges_;

    /** Every prefix a walk passed the end of, beside the string it walked from. */
    st::vector<range_watch_t> prefixes_;

    /** Every range of strings removed, its start and its end, empty for none, in removal order. */
    st::vector<range_watch_t> removed_ranges_;

    /** The strings sealed per written collection, between a stage or a commit and its landing. */
    st::vector<sealed_collection_t> sealed_;

    /** What joins this to a database transaction. */
    transaction_share_t share_;
};

#pragma endregion Transaction

} // namespace unum::ustore::strings
