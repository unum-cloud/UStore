/**
 *  @file include/ustore/strings/nvme_store.hpp
 *  @author Ash Vardanian
 *  @date October 20, 2026
 *  @brief The NVMe store of a strings collection, keyed by whole strings: strings memory runs in front of an LSM tree
 *      whose runs carry the tails of their strings, and the strings one database transaction writes and watches.
 *
 *  Every string is a key of its own, mapped onto one 16-byte key of another collection. A memory run keeps it whole on
 *  its key tape, and a run file keeps its 16-byte head and its 16-byte value in a 32-byte cell and the bytes past the
 *  head in the run's tails, which are read into memory with the index, so a lookup binary-searches the heads and
 *  compares tails only between equal heads, and answers the value from the cell, with no I/O. Two strings sharing
 *  their leading 15 bytes stay two keys everywhere: in a commit's watches, in a merge, which keeps the newest version
 *  of each, and in a sorted level, whose runs never split one head, so a level finds a head in one run.
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

#include <algorithm>   // `std::max`, `std::min`, `std::stable_sort`
#include <compare>     // `std::strong_ordering`
#include <memory>      // `std::unique_ptr`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`, `std::as_bytes`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::is_same_v`, `std::remove_cvref_t`
#include <utility>     // `std::exchange`, `std::move`

#include <smashtable/basic_hash_table.hpp> // `st::hash_map`
#include <smashtable/basic_vector.hpp>     // `st::basic_vector`
#include <stringzilla/stringzilla.hpp>     // `sz::string_view_t`

#include "ustore/blobs/nvme_collection_view.hpp" // `collection_view`, `published_view`
#include "ustore/blobs/nvme_store.hpp"           // `commit_ledger`, `stamp_pin`, `lookup_result_t`, `memory_pressure_t`
#include "ustore/blobs/nvme_database.hpp"        // `transaction_share_t`
#include "ustore/blobs/nvme_tree.hpp"            // `lsm_tree`, `collection_config_t`, `lsm_tree_publication_hook_t`
#include "ustore/strings/string_keys.hpp"        // `head_of`, `head_length_of`, `tail_of`, `head_prefix_bytes_k`
#include "ustore/strings/nvme_memory_run.hpp"    // `memory_run`, `sealed_deltas`, `kept_versions`
#include "ustore/utilities/folds.hpp"            // `last_write_fold_t`, `delta_view_t`
#include "ustore/utilities/ram_buffers.hpp"      // `local_arena_t`
#include "ustore/utilities/types.hpp"            // `status_t`, `expected`, `byte_t`

namespace unum::ustore::strings {

using blobs::collection_config_t;
using blobs::collection_id_hash_t;
using blobs::collection_id_t;
using blobs::collection_view;
using blobs::commit_ledger;
using blobs::compaction_outcome_t;
using blobs::compaction_placement_t;
using blobs::head_stamp_k;
using blobs::invalid_transaction_stamp_k;
using blobs::key_slot;
using blobs::key_tails_column_t;
using blobs::key_tails_t;
using blobs::lookup_result_t;
using blobs::lsm_level_order_t;
using blobs::lsm_tree;
using blobs::lsm_tree_publication_hook_t;
using blobs::lsm_tree_run_record_t;
using blobs::memory_pressure_t;
using blobs::memory_shape_t;
using blobs::published_view;
using blobs::scan_page_keys;
using blobs::stamp_pin;
using blobs::stored_value_size_t;
using blobs::transaction_share_t;
using blobs::unpublished_t;
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
    st::basic_vector<char> texts;

    /** Where each string sits in @c texts, in order. */
    st::basic_vector<listed_string_t> listed;

    /** A string put back together from a head and a tail before it lands in @c texts. */
    st::basic_vector<char> whole;

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
 *  A reader announces itself in its ledger's epochs, loads the published stamp and then the view, and walks immutable
 *  runs with no lock. A committer seals its strings into one memory run on its own thread, and the ledger's leader
 *  publishes a view naming it ahead of the others. Merges of memory runs, flushes, compactions and truncations follow
 *  @c blobs::nvme_store, and are serialized by the database that owns the store. A string's newest version answers it,
 *  a removal included, and every version a pinned reader or a mount needs survives a merge.
 *
 *  @tparam atomic_reference_ The reference the published view, the budget and the ledger own their words through.
 *  @tparam waiter_type_ What a core waiting on a read, a barrier or the ledger does between attempts, before it parks.
 */
template <typename volume_type_, template <typename> typename atomic_reference_, typename waiter_type_>
class nvme_store {
  public:

#pragma region Types

    /** The cell every run of this store holds: the head of a string and the 16 bytes of its value, in runs that carry
     *  the tails of their strings, which tell the strings of one head apart. */
    using key_slot_t = key_slot<st::key128_t, value_layout_t::uuid_k, key_tails_t::carried_k>;

    /** The volume every run of this store lives on. */
    using volume_t = volume_type_;

    /** What a delta of this store does, which is always a whole value or a removal. */
    using fold_t = utilities::last_write_fold_t;

    /** The key a caller addresses this store by. */
    using stored_key_t = std::string_view;

    /** The tree the flushed strings live in. */
    using tree_t = lsm_tree<key_slot_t, volume_t, atomic_reference_, waiter_type_>;

    /** One level set of that tree, which a view names. */
    using level_set_t = typename tree_t::level_set_t;

    /** One immutable run of a level. */
    using sorted_run_t = typename tree_t::sorted_run_t;

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

    /** The memory runs one view names, newest first. */
    using memory_view_t = collection_view<memory_run_t>;

    /** One delta a commit hands over: the string, its kind, and its bytes. */
    using keyed_delta_t = keyed_delta<std::string_view>;

    /** One version a merge or a flush carries over from a memory run. */
    using stamped_delta_t = stamped_delta<std::string_view>;

    /** One commit's strings for this store, sealed off every shared path by its committer, and the run the leader
     *  stamps them into, both charged to the store's budget. */
    struct sealed_commit_t {

        /** The deltas, sorted and copied, which the leader moves into @c run once it draws the stamp. */
        sealed_deltas_t deltas;

        /** The run they become, allocated by the committer, owned by the store once published. */
        std::unique_ptr<memory_run_t> run;
    };

    /** Everything a reader of this store needs, which nothing changes once it is published. */
    struct view_t {

        /** The memory runs, newest first, every stamp of which is newer than any level's. */
        memory_view_t memory_runs;

        /** The levels below them. */
        level_set_t const* levels = nullptr;

        /** The oldest stamp a reader that pins none may read at, raised by every merge to the horizon it kept. */
        transaction_stamp_t oldest_readable = unwritten_stamp_k;

        /** Its room in the ledger's list of what the leader unpublished, once a publication replaces it. */
        unpublished_t unpublished;
    };

#pragma endregion Types

#pragma region Construction

    nvme_store() noexcept = default;
    nvme_store(nvme_store const&) = delete;
    nvme_store& operator=(nvme_store const&) = delete;
    nvme_store(nvme_store&&) noexcept = default;
    nvme_store& operator=(nvme_store&&) noexcept = default;

    /**
     *  @brief An empty store over @p volume, whose memory is charged to @p constraint.
     *  @param[inout] volume The volume every run lands on, borrowed and outliving the store.
     *  @param[in] config The bounds of the memory runs and the shape of the tree below.
     *  @param[inout] constraint The budget the memory runs are charged to.
     *  @param[inout] ledger The stamps the store's writes carry and its readers pin, outliving the store.
     */
    static expected<nvme_store> mount(volume_t& volume, collection_config_t const& config, constraint_t& constraint,
                                      ledger_t& ledger) noexcept {
        nvme_store mounted;
        mounted.config_ = config;
        mounted.constraint_ = &constraint;
        mounted.ledger_ = &ledger;
        if (config.memory_runs_max_deltas == 0)
            return status_t {status_code_t::invalid_argument_k, "A collection holds at least one write in memory"};

        auto [tree, mount_tree_status] = tree_t::mount(volume, config.lsm_tree, config.memory_runs_max_bytes);
        if (mount_tree_status.failed()) return mount_tree_status;
        mounted.tree_ = std::move(tree);
        mounted.current_.reset(new (std::nothrow) view_t {});
        if (!mounted.current_) return status_t {status_code_t::out_of_memory_k, "A store could not hold a view"};
        mounted.current_->levels = &mounted.tree_.levels();
        mounted.view_.replace(*mounted.current_);
        ledger.register_view(mounted.view_pin_);
        return mounted;
    }

    /** Whether a collection of @p modality with values of @p layout is one this store keeps: the strings modality,
     *  every value of which is one 16-byte key. */
    [[nodiscard]] static bool serves(ustore_modality_t modality, value_layout_t layout) noexcept {
        return modality == ustore_modality_strings_k && layout == value_layout_t::uuid_k;
    }

    /** Returns every run to the volume and forgets every memory run, which destroys the data and which only a store no
     *  reader reaches and no commit names may. */
    status_t destroy(io_channel_t& channel) noexcept {
        current_->memory_runs = memory_view_t::of(typename memory_view_t::runs_t {});
        memory_runs_.clear();
        ledger_->bound_view(view_pin_, head_stamp_k);
        return tree_.destroy(channel);
    }

#pragma endregion Construction

#pragma region Shape

    /** The bounds this store was mounted with. */
    [[nodiscard]] collection_config_t const& config() const noexcept { return config_; }

    /** The tree the flushed strings live in, which a database compacts and names in its catalog. */
    [[nodiscard]] tree_t const& tree() const noexcept { return tree_; }

    /** Bytes every value this store admits takes: one 16-byte key. */
    [[nodiscard]] stored_value_size_t value_max_size() const noexcept { return key_slot_t::size; }

    /** How many memory runs the published view names, and how many commits they hold. */
    [[nodiscard]] memory_shape_t memory_shape() const noexcept {
        reader_t const inside = ledger_->epochs().enter();
        memory_shape_t shape;
        for (memory_run_t const* run : view_.load()->memory_runs.runs()) {
            std::uint64_t commits = 1;
            for (std::uint32_t tier = 0; tier != run->tier(); ++tier) commits *= config_.lsm_tree.level_enlarge_factor;
            shape.commits += commits;
            ++shape.runs;
        }
        return shape;
    }

    /** Deltas the memory runs hold plus key slots every level holds, one string counted once per run holding it. */
    [[nodiscard]] std::uint64_t population_count() const noexcept {
        reader_t const inside = ledger_->epochs().enter();
        view_t const& view = *view_.load();
        return memory_deltas_(view) + view.levels->population_count();
    }

    /** Bytes every run of every level takes on its volume. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept {
        reader_t const inside = ledger_->epochs().enter();
        return view_.load()->levels->size_bytes();
    }

#pragma endregion Shape

#pragma region Lookups

    /**
     *  @brief Whether anything carries @p text, copying its 16-byte value into @p value from a memory run or from a
     *      cell, with no I/O.
     *  @param[out] value The value, written only for @c written_k.
     *  @param[in] stamp The stamp a reader is pinned at, or @c head_stamp_k for the published one.
     *  @param[in] thread_index The executor thread reading, which announces on its own epoch slot.
     */
    [[nodiscard]] lookup_result_t find(std::string_view text, std::span<byte_t, key_slot_t::size> value,
                                       transaction_stamp_t stamp = head_stamp_k,
                                       std::size_t thread_index = outside_executor_k) const noexcept {
        reader_t const inside = ledger_->epochs().enter(thread_index);
        reading_t const read = reading_(stamp);
        found_t found;
        if (!newest_(*read.view, text, read.stamp, found)) return lookup_result_t::missing_k;
        if (found.delta.kind == delta_kind_t::remove_k) return lookup_result_t::removed_k;
        std::memcpy(value.data(), found.run ? found.run->key_slot(found.position).data : found.delta.bytes.data(),
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
     *  @param[out] size Bytes written, which is zero for every failure.
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
     *  @param[in] stamp A stamp a reader pins, whose versions every merge keeps, so a walk reads every page at it.
     *
     *  Every run gets a cursor over its window, newest first: the memory runs, then level zero youngest first, then
     *  the deeper levels. The page takes the smallest key among them, answers it from the first cursor holding a
     *  version at or below the stamp, and moves every cursor past it. A walk reads a page inside the ledger's epochs
     *  at a time, so a commit waits for one page at most, and starts the next past the last string of the page.
     */
    status_t read_page(std::string_view prefix, std::string_view from, transaction_stamp_t stamp,
                       std::size_t thread_index, strings_page_t& page) const noexcept {
        page.texts.clear();
        page.listed.clear();
        reader_t const inside = ledger_->epochs().enter(thread_index);
        view_t const& view = *view_.load();
        level_set_t const& levels = *view.levels;
        std::size_t runs = view.memory_runs.runs().size();
        for (std::size_t number = 0; number != levels.size(); ++number) runs += levels.level(number).size();
        st::basic_vector<cursor_t> cursors;
        if (cursors.reserve(runs) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A walk could not hold a cursor per run"};
        st::key128_t const from_head = head_of(from);
        std::string_view const from_tail = tail_of(from);
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
                cursor.end = prefix_end_in_(run, run.population_count(), prefix);
                cursor.position = std::min(lower_bound_in_(run, run.population_count(), from_head, from_tail),
                                           cursor.end);
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
            listed_string_t listed;
            bool taken = false;
            // Cursors sit newest first with disjoint stamps, so the first version at or below the stamp is the newest.
            for (cursor_t& cursor : cursors)
                for (; cursor.position != cursor.end &&
                       order_keys(cursor_head_(cursor), cursor_tail_(cursor), head, tail) == 0;
                     ++cursor.position) {
                    if (taken) continue;
                    transaction_stamp_t const version = cursor.memory ? cursor.memory->stamp(cursor.position)
                                                                      : cursor.level->stamp(cursor.position);
                    if (version > stamp) continue;
                    taken = true;
                    delta_kind_t const kind = cursor.memory ? cursor.memory->delta(cursor.position).kind
                                                            : cursor.level->kind(cursor.position);
                    if (kind == delta_kind_t::remove_k) {
                        listed.state = lookup_result_t::removed_k;
                        continue;
                    }
                    listed.state = lookup_result_t::written_k;
                    std::memcpy(&listed.value,
                                cursor.memory ? cursor.memory->delta(cursor.position).bytes.data()
                                              : cursor.level->key_slot(cursor.position).data,
                                key_slot_t::size);
                }
            if (!taken) continue;

            std::string_view text;
            if (status_t const joined = string_of_key(head, tail, page.whole, text); joined.failed()) return joined;
            listed.offset = page.texts.size();
            listed.length = text.size();
            if (page.texts.resize(listed.offset + listed.length) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A walk could not hold its page"};
            if (!text.empty()) std::memcpy(page.texts.data() + listed.offset, text.data(), text.size());
            if (page.listed.push_back(std::move(listed)) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A walk could not hold its page"};
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
     */
    expected<sealed_commit_t> seal(std::span<keyed_delta_t> deltas) const noexcept {
        for (keyed_delta_t const& each : deltas) {
            if (each.delta.kind == delta_kind_t::change_k)
                return status_t {status_code_t::invalid_argument_k, "A string takes whole values and removals"};
            if (each.delta.kind == delta_kind_t::write_k && !admits(each.delta.bytes.size()))
                return status_t {status_code_t::malformed_k, "A string maps onto one 16-byte key"};
        }
        sealed_commit_t sealed;
        if (deltas.empty()) return sealed;
        auto [sealed_deltas, seal_status] = sealed_deltas_t::seal(deltas, allocator_t {*constraint_});
        if (seal_status.failed()) return seal_status;
        sealed.deltas = std::move(sealed_deltas);
        sealed.run.reset(new (std::nothrow) memory_run_t {});
        if (!sealed.run) return status_t {status_code_t::out_of_memory_k, "No room for another memory run"};
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
        return seal(last_of_each_string_(deltas));
    }

    /** Seals a removal of every string of @p texts. */
    expected<sealed_commit_t> seal_removals(std::span<std::string_view const> texts) const noexcept {
        auto [deltas, deltas_status] = deltas_of_(texts.size());
        if (deltas_status.failed()) return deltas_status;
        for (std::size_t index = 0; index != texts.size(); ++index)
            deltas[index] = keyed_delta_t {texts[index], delta_view_t {delta_kind_t::remove_k, {}}};
        return seal(last_of_each_string_(deltas));
    }

    /** How full the memory runs of the published view would be with @p sealed beside them. */
    [[nodiscard]] memory_pressure_t memory_pressure(sealed_commit_t const& sealed) const noexcept {
        reader_t const inside = ledger_->epochs().enter();
        view_t const& view = *view_.load();
        if (view.memory_runs.runs().empty()) return memory_pressure_t::fits_k;
        std::uint64_t const deltas = memory_deltas_(view) + sealed.deltas.size();
        std::uint64_t const bytes = memory_bytes_(view) + sealed.deltas.keys_bytes() + sealed.deltas.values_bytes();
        std::uint64_t const deltas_bound = config_.memory_runs_max_deltas;
        std::uint64_t const bytes_bound = config_.memory_runs_max_bytes;
        // One flush in flight and one buffer filling behind it.
        if (deltas > 2 * deltas_bound || bytes > 2 * bytes_bound) return memory_pressure_t::stalled_k;
        if (deltas > deltas_bound || bytes > bytes_bound) return memory_pressure_t::flush_due_k;
        return memory_pressure_t::fits_k;
    }

    /**
     *  @brief Lays out the view naming @p sealed's run ahead of the current ones, so @c publish cannot fail.
     *  @note Called by the thread holding the ledger's leader word, which then publishes or forgets it before it lets
     *      the word go.
     */
    status_t prepare(sealed_commit_t& sealed) noexcept {
        forget_prepared();
        if (!sealed.run) return {};
        auto [view, view_status] = next_view_(sealed.run.get(), current_->memory_runs.runs(),
                                              current_->oldest_readable);
        if (view_status.failed() || memory_runs_.reserve(memory_runs_.size() + 1) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "No room to prepare a commit"};
        view->levels = current_->levels;
        prepared_view_ = std::move(view);
        return {};
    }

    /** Stamps @p sealed's run with @p stamp, publishes the view @c prepare laid out, and takes the run over, which the
     *  leader does before it publishes @p stamp itself, so a reader at the stamp finds the run. */
    void publish(sealed_commit_t& sealed, transaction_stamp_t stamp) noexcept {
        if (!sealed.run) return;
        expected<memory_run_t> stamped = memory_run_t::stamped(std::move(sealed.deltas), stamp,
                                                               allocator_t {*constraint_});
        if (stamped) *sealed.run = std::move(*stamped);
        // The durable horizon stays below the oldest commit that only memory holds.
        if (current_->memory_runs.runs().empty()) ledger_->bound_view(view_pin_, stamp - 1);
        replace_view_(std::move(prepared_view_));
        [[maybe_unused]] st::status_t const owned = memory_runs_.push_back(st::assume_reserved, std::move(sealed.run));
    }

    /** Drops the view @c prepare laid out, which the leader does for a commit it then refuses. */
    void forget_prepared() noexcept { prepared_view_.reset(); }

    /** Whether a commit newer than @p from wrote @p text, which only the string's newest version decides. */
    [[nodiscard]] bool written_since(std::string_view text, transaction_stamp_t from) const noexcept {
        reader_t const inside = ledger_->epochs().enter();
        view_t const& view = *view_.load();
        // Memory runs sit newest first and above every level, so one no newer than the watch ends the search.
        for (memory_run_t const* run : view.memory_runs.runs()) {
            if (run->newest_stamp() <= from) return false;
            std::size_t const position = run->lower_bound(text);
            if (position != run->size() && run->stored_key(position) == text) return run->stamp(position) > from;
        }
        found_t found;
        if (!newest_in_levels_(*view.levels, head_of(text), tail_of(text), head_stamp_k, found)) return false;
        return found.run->stamp(found.position) > from;
    }

    /** Whether a commit newer than @p from wrote any string from @p lower to @p upper inclusive, which a walk over them
     *  watches, so a string written where it found none conflicts too. */
    [[nodiscard]] bool range_written_since(std::string_view lower, std::string_view upper,
                                           transaction_stamp_t from) const noexcept {
        if (upper < lower) return false;
        reader_t const inside = ledger_->epochs().enter();
        view_t const& view = *view_.load();
        if (view.memory_runs.range_written_since(lower, upper, from)) return true;
        st::key128_t const upper_head = head_of(upper);
        std::string_view const upper_tail = tail_of(upper);
        return any_level_run_(*view.levels, [&](sorted_run_t const& run) noexcept {
            if (run.newest_stamp() <= from) return false;
            for (std::uint64_t position = lower_bound_in_(run, run.population_count(), head_of(lower), tail_of(lower));
                 position != run.population_count() &&
                 order_keys(run.key_slot(position).stored_key, run.tail(position), upper_head, upper_tail) <= 0;
                 ++position)
                if (run.stamp(position) > from) return true;
            return false;
        });
    }

    /** Whether a commit newer than @p from wrote any string starting with @p prefix and not below @p lower, which a
     *  walk to the end of the prefix watches. */
    [[nodiscard]] bool prefix_written_since(std::string_view prefix, std::string_view lower,
                                            transaction_stamp_t from) const noexcept {
        std::string_view const first = std::max(lower, prefix);
        reader_t const inside = ledger_->epochs().enter();
        view_t const& view = *view_.load();
        for (memory_run_t const* run : view.memory_runs.runs()) {
            if (run->newest_stamp() <= from) break;
            std::size_t const end = run->prefix_end(prefix);
            for (std::size_t position = run->lower_bound(first); position < end; ++position)
                if (run->stamp(position) > from) return true;
        }
        return any_level_run_(*view.levels, [&](sorted_run_t const& run) noexcept {
            if (run.newest_stamp() <= from) return false;
            std::uint64_t const end = prefix_end_in_(run, run.population_count(), prefix);
            for (std::uint64_t position = lower_bound_in_(run, run.population_count(), head_of(first), tail_of(first));
                 position < end; ++position)
                if (run.stamp(position) > from) return true;
            return false;
        });
    }

    /** Opens the runs @p runs names as this store's tree, which a mount does before anything reads. */
    status_t restore(io_channel_t& channel, std::span<lsm_tree_run_record_t const> runs) noexcept {
        publication_t publication;
        publication.owner = this;
        return tree_.restore(channel, runs, publication.hook());
    }

#pragma endregion Writes

#pragma region Maintenance

    /**
     *  @brief Merges F memory runs of one tier into one run of the next tier, F being the level enlarge factor, until
     *      no tier holds F runs, as @c blobs::nvme_store does.
     *  @note The owner serializes it with flushes, compactions and truncations, and the calling thread is inside no
     *      epoch.
     */
    status_t merge_memory_runs() noexcept {
        std::size_t const fan_in = config_.lsm_tree.level_enlarge_factor;
        st::basic_vector<memory_run_t const*> group;
        if (group.resize(fan_in) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A merge could not list its memory runs"};
        for (;;) {
            {
                reader_t const inside = ledger_->epochs().enter();
                std::span<memory_run_t const* const> const due = due_group_(view_.load()->memory_runs.runs());
                if (due.empty()) return {};
                // Only this store's owner frees memory runs, so these outlive the reader.
                for (std::size_t index = 0; index != fan_in; ++index) group[index] = due[index];
            }

            st::basic_vector<transaction_stamp_t> readers;
            if (status_t const listed = ledger_->pinned_readers(readers); listed.failed()) return listed;
            compaction_stamps_t survival;
            survival.pinned = readers.data();
            survival.pinned_count = readers.size();
            // Nothing a memory run holds is durable, so no mount needs a version; a head reader needs those above this.
            survival.horizon = ledger_->published();
            stamped_deltas_t versions;
            utilities::local_arena_t folded;
            utilities::arena_allocator<byte_t> folded_bytes = folded.allocator();
            if (status_t const kept = kept_versions(std::span<memory_run_t const* const> {group}, survival, fold_,
                                                    versions, folded_bytes);
                kept.failed())
                return kept;
            auto [merged, merged_status] = memory_run_t::merged(std::span<stamped_delta_t const> {versions},
                                                                group.back()->oldest_stamp(), group.back()->tier(),
                                                                allocator_t {*constraint_});
            if (merged_status.failed()) return merged_status;
            std::unique_ptr<memory_run_t> merged_run {new (std::nothrow) memory_run_t {std::move(merged)}};
            // The replaced runs, freed here once the publication waited out every reader that could hold one.
            st::basic_vector<std::unique_ptr<memory_run_t>> away;
            if (!merged_run || away.reserve(fan_in) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "No room to merge memory runs"};
            if (status_t const published = publish_merged_(std::span<memory_run_t const* const> {group},
                                                           std::move(merged_run), away, survival.horizon);
                published.failed())
                return published;
        }
    }

    /** Whether the memory runs hold the bounds they were mounted with, so a flush is due. */
    [[nodiscard]] bool flush_due() const noexcept {
        reader_t const inside = ledger_->epochs().enter();
        return flush_due_(*view_.load());
    }

    /** Whether the memory runs are due for a merge, holding F runs of tier zero, or for a flush. */
    [[nodiscard]] bool maintenance_due() const noexcept {
        reader_t const inside = ledger_->epochs().enter();
        view_t const& view = *view_.load();
        std::size_t newest = 0;
        for (memory_run_t const* run : view.memory_runs.runs()) {
            if (run->tier() != 0) break;
            ++newest;
        }
        return newest >= config_.lsm_tree.level_enlarge_factor || flush_due_(view);
    }

    /**
     *  @brief Writes the memory runs of the published view as one run of level zero, heads in its cells and tails in
     *      its tails, with no lock held, then publishes a view naming that run instead of exactly those runs.
     *  @note The owner serializes flushes, compactions and truncations, and the calling thread is inside no epoch.
     */
    status_t flush(io_channel_t& channel) noexcept {
        publication_t publication;
        publication.owner = this;
        {
            reader_t const inside = ledger_->epochs().enter();
            std::span<memory_run_t const* const> const runs = view_.load()->memory_runs.runs();
            if (runs.empty()) return {};
            // Only this store's owner frees memory runs, merging or flushing one at a time, so these outlive the
            // reader.
            if (publication.flushed.resize(runs.size()) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A flush could not list its memory runs"};
            for (std::size_t index = 0; index != runs.size(); ++index) publication.flushed[index] = runs[index];
        }

        // A replaced version survives for a pinned reader, or for a mount promising the durable horizon this flush
        // leaves; at or below both, every stamp reads alike, so the run packs only above.
        st::basic_vector<transaction_stamp_t> readers;
        if (status_t const listed = ledger_->pinned_readers(readers); listed.failed()) return listed;
        compaction_stamps_t survival;
        survival.pinned = readers.data();
        survival.pinned_count = readers.size();
        survival.horizon = ledger_->horizon_besides(view_pin_);
        survival.floor = ledger_->floor_besides(view_pin_);
        publication.oldest_readable = survival.horizon;

        stamped_deltas_t versions;
        utilities::local_arena_t folded;
        utilities::arena_allocator<byte_t> folded_bytes = folded.allocator();
        if (status_t const kept = kept_versions(std::span<memory_run_t const* const> {publication.flushed}, survival,
                                                fold_, versions, folded_bytes);
            kept.failed())
            return kept;
        drained_t drained;
        if (status_t const laid = drain_(std::span<stamped_delta_t const> {versions}, drained); laid.failed())
            return laid;
        key_tails_column_t const tails = drained.tails.empty()
                                             ? key_tails_column_t {}
                                             : key_tails_column_t {
                                                   std::span<std::uint32_t const> {drained.tail_offsets},
                                                   std::span<byte_t const> {drained.tails}};
        return tree_.flush(channel, std::span<key_slot_t const> {drained.key_slots},
                           std::span<std::uint64_t const> {drained.stamps},
                           std::span<std::uint8_t const> {drained.kinds}, survival.floor, {}, key_slot_t::size,
                           publication.hook(), tails);
    }

    /** Whether any level of the tree is over the bytes its number admits. */
    [[nodiscard]] bool needs_compaction() const noexcept {
        reader_t const inside = ledger_->epochs().enter();
        return tree_.needs_compaction(*view_.load()->levels);
    }

    /**
     *  @brief Merges the shallowest level that is over its bounds into the level below it, on the host.
     *  @param[inout] reading_channel The channel the merge gathers source values on, held by the calling thread.
     *  @param[inout] writing_channel The channel the merge writes its new runs on, which is not @p reading_channel.
     *  @param[in] published What to call once the merged set is live and before a consumed run is dropped.
     *
     *  The devices the executor named go unused, as no device compaction is compiled for keys carrying tails.
     */
    expected<compaction_outcome_t> compact(
        io_channel_t& reading_channel, io_channel_t& writing_channel, std::span<int const>,
        lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {}) noexcept {
        // A pinned reader keeps the version it reads, and the durable horizon keeps the version a mount falls back to.
        st::basic_vector<transaction_stamp_t> readers;
        if (status_t const listed = ledger_->pinned_readers(readers); listed.failed()) return listed;
        compaction_stamps_t stamps;
        stamps.pinned = readers.data();
        stamps.pinned_count = readers.size();
        stamps.horizon = ledger_->durable_stamp();
        stamps.floor = ledger_->floor();
        publication_t publication;
        publication.owner = this;
        publication.oldest_readable = stamps.horizon;
        publication.chained = &published;
        return tree_.compact(reading_channel, writing_channel, compaction_placement_t::host_k, {}, nullptr, fold_,
                             publication.hook(), stamps);
    }

    /** Drops every version a mount found stamped above @p ceiling, which only a store holding no memory run takes. */
    status_t truncate_above(io_channel_t& reading_channel, io_channel_t& writing_channel, transaction_stamp_t ceiling,
                            lsm_tree_publication_hook_t const& published = lsm_tree_publication_hook_t {}) noexcept {
        {
            reader_t const inside = ledger_->epochs().enter();
            view_t const& taken = *view_.load();
            if (!taken.memory_runs.runs().empty())
                return {status_code_t::invalid_argument_k, "A store truncates only its runs"};
            if (taken.levels->newest_stamp() <= ceiling) return {};
        }
        publication_t publication;
        publication.owner = this;
        publication.chained = &published;
        return tree_.truncate_above(reading_channel, writing_channel, ceiling, fold_, publication.hook());
    }

#pragma endregion Maintenance

  private:

#pragma region Private Types

    /** One reader inside the ledger's epochs, announced until it dies. */
    using reader_t = typename ledger_t::epochs_t::reader_t;

    /** The versions one merge or one flush carries over, before and after they are sorted into a run's order. */
    using stamped_deltas_t = st::basic_vector<stamped_delta_t>;

    /** The deltas one batch of its own hands over. */
    using deltas_t = st::basic_vector<keyed_delta_t, constrained_allocator<keyed_delta_t, atomic_reference_>>;

    /** The view a read walks and the stamp it reads at. */
    struct reading_t {

        /** The view loaded after the stamp, which names every commit at or below it. */
        view_t const* view = nullptr;

        /** The stamp the read answers at. */
        transaction_stamp_t stamp = head_stamp_k;
    };

    /** The newest version of one string a read found: what it does, and its bytes in a memory run or its position in a
     *  run of a level. */
    struct found_t {

        /** What the version does, and its bytes when a memory run holds it. */
        delta_view_t delta;

        /** The run of a level holding it, or null for a memory run. */
        sorted_run_t const* run = nullptr;

        /** Its sorted position inside @c run. */
        std::uint64_t position = 0;
    };

    /** Where one run a walk merges has got to, and where its window ends. */
    struct cursor_t {

        /** The memory run, or null for a run of a level. */
        memory_run_t const* memory = nullptr;

        /** The run of a level, or null for a memory run. */
        sorted_run_t const* level = nullptr;

        /** The next sorted position inside the run. */
        std::uint64_t position = 0;

        /** The first sorted position past the window. */
        std::uint64_t end = 0;
    };

    /** The columns one flush writes: cells holding the values in ascending order, their stamps, kinds, and tails. */
    struct drained_t {
        st::basic_vector<key_slot_t> key_slots;
        st::basic_vector<std::uint64_t> stamps;
        st::basic_vector<std::uint8_t> kinds;
        st::basic_vector<std::uint32_t> tail_offsets;
        st::basic_vector<byte_t> tails;
    };

    /** What a publication of a new level set hands its hooks, as @c blobs::nvme_store's does. */
    struct publication_t {

        /** The store publishing. */
        nvme_store* owner = nullptr;

        /** The memory runs a flush wrote out, newest first, as the view it read named them; none for a merge. */
        st::basic_vector<memory_run_t const*> flushed;

        /** The horizon the merge kept, which the published view raises its oldest readable stamp to. */
        transaction_stamp_t oldest_readable = unwritten_stamp_k;

        /** The view laid out under the leader word just before the set becomes current. */
        std::unique_ptr<view_t> next;

        /** The flushed runs, taken over under the leader word and freed once no reader can hold one. */
        st::basic_vector<std::unique_ptr<memory_run_t>> freed;

        /** The hook of whoever records the new set, called last, or null for none. */
        lsm_tree_publication_hook_t const* chained = nullptr;

        /** The hooks a tree calls with this publication as their context. */
        [[nodiscard]] lsm_tree_publication_hook_t hook() noexcept {
            lsm_tree_publication_hook_t hook;
            hook.prepare = &prepare_levels_;
            hook.notify = &publish_levels_;
            hook.context = this;
            return hook;
        }
    };

#pragma endregion Private Types

#pragma region Private Methods

    /** Deltas every memory run of @p view holds. */
    [[nodiscard]] static std::size_t memory_deltas_(view_t const& view) noexcept {
        std::size_t deltas = 0;
        for (memory_run_t const* run : view.memory_runs.runs()) deltas += run->size();
        return deltas;
    }

    /** String and value bytes every memory run of @p view holds, 16 per value. */
    [[nodiscard]] static std::uint64_t memory_bytes_(view_t const& view) noexcept {
        std::uint64_t bytes = 0;
        for (memory_run_t const* run : view.memory_runs.runs()) bytes += run->keys_bytes() + run->values_bytes();
        return bytes;
    }

    /** Whether the memory runs of @p view hold the bounds they were mounted with. */
    [[nodiscard]] bool flush_due_(view_t const& view) const noexcept {
        return memory_deltas_(view) >= config_.memory_runs_max_deltas ||
               memory_bytes_(view) >= config_.memory_runs_max_bytes;
    }

    /** The view a read at @p stamp walks and the stamp it answers at, as @c blobs::nvme_store reads: a head read
     *  retries past a merge that raised the oldest readable stamp above the one it took. */
    [[nodiscard]] reading_t reading_(transaction_stamp_t stamp) const noexcept {
        for (;;) {
            transaction_stamp_t const at = stamp == head_stamp_k ? ledger_->published() : stamp;
            view_t const* const view = view_.load();
            if (stamp != head_stamp_k || at >= view->oldest_readable) return reading_t {view, at};
        }
    }

    /** The head of the key at sorted @p position of a memory run. */
    [[nodiscard]] static st::key128_t const& head_at_(memory_run_t const& run, std::uint64_t position) noexcept {
        return run.head(static_cast<std::size_t>(position));
    }

    /** The tail of the key at sorted @p position of a memory run. */
    [[nodiscard]] static std::string_view tail_at_(memory_run_t const& run, std::uint64_t position) noexcept {
        return tail_of(run.stored_key(static_cast<std::size_t>(position)));
    }

    /** The head of the key at sorted @p position of a run of a level. */
    [[nodiscard]] static st::key128_t const& head_at_(sorted_run_t const& run, std::uint64_t position) noexcept {
        return run.key_slot(position).stored_key;
    }

    /** The tail of the key at sorted @p position of a run of a level. */
    [[nodiscard]] static std::string_view tail_at_(sorted_run_t const& run, std::uint64_t position) noexcept {
        return run.tail(position);
    }

    /** The first of the @p count sorted positions of @p run whose key is not below @p head and @p tail. */
    template <typename run_type_>
    [[nodiscard]] static std::uint64_t lower_bound_in_(run_type_ const& run, std::uint64_t count,
                                                       st::key128_t const& head, std::string_view tail) noexcept {
        std::uint64_t low = 0;
        std::uint64_t high = count;
        while (low != high) {
            std::uint64_t const middle = low + (high - low) / 2;
            if (order_keys(head_at_(run, middle), tail_at_(run, middle), head, tail) < 0) low = middle + 1;
            else high = middle;
        }
        return low;
    }

    /** The first of the @p count sorted positions of @p run past every string starting with @p prefix, as
     *  @c sealed_deltas::prefix_end finds it. */
    template <typename run_type_>
    [[nodiscard]] static std::uint64_t prefix_end_in_(run_type_ const& run, std::uint64_t count,
                                                      std::string_view prefix) noexcept {
        std::string_view const leading = prefix.substr(0, std::min(prefix.size(), head_prefix_bytes_k));
        std::string_view const trailing = tail_of(prefix);
        std::uint64_t low = 0;
        std::uint64_t high = count;
        while (low != high) {
            std::uint64_t const middle = low + (high - low) / 2;
            st::key128_t const& head = head_at_(run, middle);
            std::byte head_bytes[sizeof(st::key128_t)];
            head.to_bytes(head_bytes);
            std::string_view const head_leading {reinterpret_cast<char const*>(head_bytes), leading.size()};
            int const order = head_leading.compare(leading);
            // Only a string longer than its head can reach past the leading bytes of a longer prefix.
            bool const within_or_below =
                order < 0 || (order == 0 && (trailing.empty() || head_length_of(head) != long_head_length_k ||
                                             tail_at_(run, middle).substr(0, trailing.size()) <= trailing));
            if (within_or_below) low = middle + 1;
            else high = middle;
        }
        return low;
    }

    /** Whether the key at sorted @p position of @p run is the string of @p head and @p tail. */
    template <typename run_type_>
    [[nodiscard]] static bool same_key_at_(run_type_ const& run, std::uint64_t position, st::key128_t const& head,
                                           std::string_view tail) noexcept {
        return head_at_(run, position) == head && tail_at_(run, position) == tail;
    }

    /** The run of the sorted @p level whose heads cover @p head, which only one run's may, or null. */
    [[nodiscard]] static sorted_run_t const* run_holding_(typename tree_t::level_t const& level,
                                                          st::key128_t const& head) noexcept {
        std::size_t low = 0;
        std::size_t high = level.size();
        while (low != high) {
            std::size_t const middle = low + (high - low) / 2;
            sorted_run_t const& run = level.run(middle);
            if (run.upper_stored_key() < head) low = middle + 1;
            else if (head < run.lower_stored_key()) high = middle;
            else return &run;
        }
        return nullptr;
    }

    /** Whether @p run holds a version of the string of @p head and @p tail stamped at or below @p at, answering the
     *  newest one in @p found. */
    [[nodiscard]] static bool version_in_(sorted_run_t const& run, st::key128_t const& head, std::string_view tail,
                                          transaction_stamp_t at, found_t& found) noexcept {
        std::uint64_t const count = run.population_count();
        if (count == 0 || head < run.lower_stored_key() || run.upper_stored_key() < head) return false;
        for (std::uint64_t position = lower_bound_in_(run, count, head, tail);
             position != count && same_key_at_(run, position, head, tail); ++position)
            if (run.stamp(position) <= at) {
                found = found_t {delta_view_t {run.kind(position), {}}, &run, position};
                return true;
            }
        return false;
    }

    /** Whether any level of @p levels holds a version of the string of @p head and @p tail at or below @p at,
     *  answering the newest in @p found: the youngest run of level zero first, then each sorted level's one run. */
    [[nodiscard]] static bool newest_in_levels_(level_set_t const& levels, st::key128_t const& head,
                                                std::string_view tail, transaction_stamp_t at,
                                                found_t& found) noexcept {
        for (std::size_t number = 0; number != levels.size(); ++number) {
            auto const& level = levels.level(number);
            if (level.order() == lsm_level_order_t::overlapping_k) {
                for (std::size_t index = level.size(); index-- != 0;)
                    if (version_in_(level.run(index), head, tail, at, found)) return true;
                continue;
            }
            if (sorted_run_t const* const run = run_holding_(level, head))
                if (version_in_(*run, head, tail, at, found)) return true;
        }
        return false;
    }

    /** Whether any memory run or level of @p view holds a version of @p text at or below @p at, answering the newest in
     *  @p found. */
    [[nodiscard]] static bool newest_(view_t const& view, std::string_view text, transaction_stamp_t at,
                                      found_t& found) noexcept {
        if (view.memory_runs.find(text, at, found.delta)) return true;
        return newest_in_levels_(*view.levels, head_of(text), tail_of(text), at, found);
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
    [[nodiscard]] static st::key128_t const& cursor_head_(cursor_t const& cursor) noexcept {
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

    /** A view naming @p newest, when not null, ahead of @p older, below levels its caller fills in. */
    static expected<std::unique_ptr<view_t>> next_view_(memory_run_t const* newest,
                                                        std::span<memory_run_t const* const> older,
                                                        transaction_stamp_t oldest_readable) noexcept {
        std::size_t const ahead = newest != nullptr ? 1 : 0;
        typename memory_view_t::runs_t runs;
        if (runs.resize(older.size() + ahead) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room to lay out a view"};
        if (newest != nullptr) runs[0] = newest;
        for (std::size_t index = 0; index != older.size(); ++index) runs[index + ahead] = older[index];
        std::unique_ptr<view_t> view {new (std::nothrow)
                                          view_t {memory_view_t::of(std::move(runs)), nullptr, oldest_readable}};
        if (!view) return status_t {status_code_t::out_of_memory_k, "No room to lay out a view"};
        return view;
    }

    /** Publishes @p newer in place of the current view, which only the thread holding the leader word does, and hands
     *  the view it replaces to the ledger, which frees it once that thread lets the word go and its readers left. */
    void replace_view_(std::unique_ptr<view_t> newer) noexcept {
        view_.replace(*newer);
        view_t* const older = std::exchange(current_, std::move(newer)).release();
        ledger_->unpublish(older->unpublished, older, &destroy_view_);
    }

    static void destroy_view_(void* view) noexcept { delete static_cast<view_t*>(view); }

    /** Takes the leader word and lays out the view the tree's next level set will be named by: the current memory runs
     *  but those the publication wrote out, which are the oldest, since commits only prepend. */
    static status_t prepare_levels_(void* context) noexcept {
        publication_t& publication = *static_cast<publication_t*>(context);
        nvme_store& owner = *publication.owner;
        owner.ledger_->lead();
        std::span<memory_run_t const* const> const runs = owner.current_->memory_runs.runs();
        std::size_t const kept = runs.size() - publication.flushed.size();
        auto [next, next_status] = next_view_(nullptr, runs.first(kept),
                                              std::max(owner.current_->oldest_readable, publication.oldest_readable));
        if (next_status.succeeded() && publication.freed.reserve(publication.flushed.size()) != st::status_t::success_k)
            next_status = status_t {status_code_t::out_of_memory_k, "A flush could not take its memory runs over"};
        if (next_status.failed()) {
            owner.ledger_->stop_leading();
            return next_status;
        }
        publication.next = std::move(next);
        return {};
    }

    /** Publishes the view @c prepare_levels_ laid out, takes the flushed runs over, bounds the durable stamp by what
     *  memory still holds, lets the leader word go, frees the runs, and calls the hook it chains to. */
    static status_t publish_levels_(void* context) noexcept {
        publication_t& publication = *static_cast<publication_t*>(context);
        nvme_store& owner = *publication.owner;
        publication.next->levels = &owner.tree_.levels();
        owner.replace_view_(std::move(publication.next));
        if (!publication.flushed.empty()) {
            // The flushed runs are the oldest, which the store owns first.
            for (std::size_t index = 0; index != publication.flushed.size(); ++index) [[maybe_unused]]
                st::status_t const taken = publication.freed.push_back(st::assume_reserved,
                                                                       std::move(owner.memory_runs_[index]));
            for (std::size_t index = 0; index != publication.flushed.size(); ++index) owner.memory_runs_.erase(0);
            std::span<memory_run_t const* const> const kept = owner.current_->memory_runs.runs();
            owner.ledger_->bound_view(owner.view_pin_, kept.empty() ? head_stamp_k : kept.back()->oldest_stamp() - 1);
            publication.flushed.clear();
        }
        // Every reader that could hold a flushed run entered before the view replaced above, which this waits out.
        owner.ledger_->stop_leading();
        publication.freed.clear();
        lsm_tree_publication_hook_t const* const chained = publication.chained;
        if (chained == nullptr || chained->notify == nullptr) return {};
        return chained->notify(chained->context);
    }

    /** Room for @p count deltas of one batch of its own. */
    expected<deltas_t> deltas_of_(std::size_t count) const noexcept {
        deltas_t deltas {constrained_allocator<keyed_delta_t, atomic_reference_> {*constraint_}};
        if (deltas.resize(count) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "No room to hand a batch over"};
        return deltas;
    }

    /** Keeps the last delta of every string of @p deltas, which a batch naming a string twice takes. */
    [[nodiscard]] static std::span<keyed_delta_t> last_of_each_string_(deltas_t& deltas) noexcept {
        std::stable_sort(deltas.begin(), deltas.end(), [](keyed_delta_t const& first, keyed_delta_t const& second) {
            return first.stored_key < second.stored_key;
        });
        std::size_t kept = 0;
        for (std::size_t index = 0; index != deltas.size(); ++index) {
            bool const repeated = index + 1 != deltas.size() &&
                                  deltas[index + 1].stored_key == deltas[index].stored_key;
            if (!repeated) deltas[kept++] = deltas[index];
        }
        return std::span<keyed_delta_t> {deltas.data(), kept};
    }

    /** The oldest F runs of the lowest tier holding F of @p runs, newest first, or none. */
    [[nodiscard]] std::span<memory_run_t const* const> due_group_(
        std::span<memory_run_t const* const> runs) const noexcept {
        std::size_t const fan_in = config_.lsm_tree.level_enlarge_factor;
        for (std::size_t first = 0; first != runs.size();) {
            std::size_t end = first;
            while (end != runs.size() && runs[end]->tier() == runs[first]->tier()) ++end;
            if (end - first >= fan_in) return runs.subspan(end - fan_in, fan_in);
            first = end;
        }
        return {};
    }

    /** Takes the leader word, publishes a view naming @p merged where exactly the runs of @p group sat, moves those
     *  runs into @p away, and lets the word go, which returns once no reader can reach them. */
    status_t publish_merged_(std::span<memory_run_t const* const> group, std::unique_ptr<memory_run_t> merged,
                             st::basic_vector<std::unique_ptr<memory_run_t>>& away,
                             transaction_stamp_t horizon) noexcept {
        ledger_->lead();
        std::span<memory_run_t const* const> const runs = current_->memory_runs.runs();
        std::size_t first = 0;
        while (runs[first] != group.front()) ++first;
        typename memory_view_t::runs_t laid_out;
        if (laid_out.resize(runs.size() - group.size() + 1) != st::status_t::success_k) {
            ledger_->stop_leading();
            return {status_code_t::out_of_memory_k, "No room to lay out a merged view"};
        }
        for (std::size_t index = 0; index != first; ++index) laid_out[index] = runs[index];
        laid_out[first] = merged.get();
        for (std::size_t index = first + group.size(); index != runs.size(); ++index)
            laid_out[index - group.size() + 1] = runs[index];
        std::unique_ptr<view_t> next {new (std::nothrow) view_t {memory_view_t::of(std::move(laid_out)),
                                                                 current_->levels,
                                                                 std::max(current_->oldest_readable, horizon),
                                                                 {}}};
        if (!next) {
            ledger_->stop_leading();
            return {status_code_t::out_of_memory_k, "No room to lay out a merged view"};
        }
        // The runs the store owns run oldest first, opposite to the view, so the group's oldest sits here.
        std::size_t const oldest = runs.size() - first - group.size();
        replace_view_(std::move(next));
        for (std::size_t index = 0; index != group.size(); ++index) [[maybe_unused]]
            st::status_t const taken = away.push_back(st::assume_reserved, std::move(memory_runs_[oldest + index]));
        memory_runs_[oldest] = std::move(merged);
        for (std::size_t index = 1; index != group.size(); ++index) memory_runs_.erase(oldest + 1);
        ledger_->stop_leading();
        return {};
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The bounds this store was mounted with. */
    collection_config_t config_ {};

    /** The budget the memory runs are charged to, null until the store is mounted. */
    constraint_t* constraint_ = nullptr;

    /** The tree the flushed strings live in. */
    tree_t tree_;

    /** The stamps this store's writes carry and its readers pin, borrowed and outliving the store. */
    ledger_t* ledger_ = nullptr;

    /** Registers the published view with the ledger, bounding the durable stamp below the oldest memory run. */
    pin_t view_pin_;

    /** The view readers load, which @c current_ owns. */
    published_view<view_t, atomic_reference_> view_;

    /** The view published last, owned here and replaced only under the ledger's leader word. */
    std::unique_ptr<view_t> current_;

    /** Every memory run the current view names, oldest first. */
    st::basic_vector<std::unique_ptr<memory_run_t>> memory_runs_;

    /** The view a leader laid out for the commit it prepared, published or forgotten before it lets the word go. */
    std::unique_ptr<view_t> prepared_view_;

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

    /** What this transaction wrote under @p text of @p collection, viewing its value until the next write, and
     *  whether it wrote anything there. */
    [[nodiscard]] bool uncommitted(collection_id_t collection, std::string_view text,
                                   delta_view_t& found) const noexcept {
        bool written = false;
        writes_.probe_to_find(probe_t {collection, text}, [&](auto const& slot) noexcept {
            found = delta_of_(slot.value_ref());
            written = true;
        });
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
    };

    /** Hashes a string of a collection through StringZilla, the same whether a tape or a caller holds it. */
    struct place_hash_t {
        st::basic_vector<char> const* tape = nullptr;

        [[nodiscard]] std::size_t hash_(collection_id_t const& collection, std::string_view text) const noexcept {
            // StringZilla takes no null pointer, which an empty string may view.
            std::size_t const hashed = text.empty() ? 0 : sz::string_view_t {text}.hash();
            return hashed ^ (collection_id_hash_t {}(collection) * 0x9E3779B97F4A7C15ull);
        }
        std::size_t operator()(place_t const& place) const noexcept {
            return hash_(place.collection, std::string_view {tape->data() + place.offset, place.size});
        }
        std::size_t operator()(probe_t const& probe) const noexcept { return hash_(probe.collection, probe.text); }
    };

    /** Tells two strings of a collection apart, whether a tape or a caller holds them. */
    struct place_equals_t {
        st::basic_vector<char> const* tape = nullptr;

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
        st::basic_vector<keyed_delta_t> deltas;
        st::basic_vector<collection_id_t> collections;
        status_t listed;
        [[maybe_unused]] st::status_t const walked = writes_.for_each([&](auto const& slot) noexcept {
            collection_id_t const collection = slot.key_ref().collection;
            if (listed.failed()) return;
            for (collection_id_t const& seen : collections)
                if (seen == collection) return;
            listed = growth_status_(collections.push_back(collection_id_t {collection}));
        });
        if (listed.failed()) return listed;

        for (collection_id_t const& collection : collections) {
            deltas.clear();
            for_each_uncommitted(collection, [&](std::string_view text, delta_view_t delta) noexcept {
                if (listed.succeeded()) listed = growth_status_(deltas.push_back(keyed_delta_t {text, delta}));
            });
            if (listed.failed()) return listed;
            sealed_collection_t sealed;
            sealed.collection = collection;
            if (status_t const made = with_store_(collection,
                                                  [&](store_t& store) noexcept -> status_t {
                                                      auto [commit, seal_status] = store.seal(
                                                          std::span<keyed_delta_t> {deltas});
                                                      if (seal_status.failed()) return seal_status;
                                                      sealed.commit = std::move(commit);
                                                      return {};
                                                  });
                made.failed())
                return made;
            if (status_t const room = database_->make_room(collection, sealed.commit); room.failed()) return room;
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
            if (status_t const prepared = owner.with_store_(
                    sealed.collection, [&](store_t& store) noexcept { return store.prepare(sealed.commit); });
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
    st::basic_vector<char> tape_;

    /** The last write of every string, located on the tape. */
    writes_t writes_;

    /** Every string read and watched. */
    st::basic_vector<read_watch_t> reads_;

    /** Every closed range of strings a walk listed. */
    st::basic_vector<range_watch_t> ranges_;

    /** Every prefix a walk passed the end of, beside the string it walked from. */
    st::basic_vector<range_watch_t> prefixes_;

    /** The strings sealed per written collection, between a stage or a commit and its landing. */
    st::basic_vector<sealed_collection_t> sealed_;

    /** What joins this to a database transaction. */
    transaction_share_t share_;
};

#pragma endregion Transaction

} // namespace unum::ustore::strings
