/**
 *  @file src/nvme/collection.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief A write buffer and a value cache in front of an LSM tree of sorted runs.
 *
 *  Three places can answer a key, and they are consulted newest first: the write buffer holds what has
 *  not been flushed, the value cache holds what a point read already paid for, and the tree holds the
 *  rest. Only the third moves bytes, and it moves them through one snapshot of the level set, so a
 *  batched select reaches the lease's depth rather than the size of whatever batch it was handed.
 *
 *  Nothing here allocates an I/O buffer. A caller's span is never registered memory, so the bytes are
 *  copied out of the lane's own blocks either way, and that one copy is the whole of the read path.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm> // `std::sort`
#include <span>      // `std::span`
#include <utility>   // `std::move`

#include <smashtable/basic_hash_table.hpp> // `st::hash_map`, `st::assume_reserved_t`
#include <smashtable/basic_vector.hpp>     // `st::basic_vector`

#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>  // `byte_t`

#include "nvme/collection_snapshot.hpp"   // `collection_snapshot`, `select_result_t`
#include "nvme/config.hpp"                // `collection_config_t`
#include "nvme/constrained_allocator.hpp" // `resource_constraint_t`, `constrained_allocator`
#include "nvme/io_lane.hpp"               // `io_lane_t`
#include "nvme/lsm_sorted_run.hpp"        // `sorted_run_layout`
#include "nvme/lsm_tree.hpp"              // `lsm_tree`, `compaction_outcome_t`
#include "nvme/stored_value.hpp"          // `stored_value_size_t`, `empty_stored_value_offset_k`
#include "nvme/types.hpp"                 // `value_layout_t`, `is_inline`, `is_fixed`
#include "nvme/value_cache.hpp"           // `value_cache`

namespace unum::ustore::nvme {

#pragma region Collection

/**
 *  @brief A write buffer and a value cache in front of an LSM tree of sorted runs.
 *
 *  The volume, the budget and every lane are borrowed and must outlive the collection. A lane belongs
 *  to the thread holding it for as long as it holds it, so every call that can move bytes takes one
 *  rather than reaching for a shared handle.
 */
template <typename key_slot_type_, typename volume_type_>
class collection {
  public:

#pragma region Types

    /** The key slot every run of this collection holds. */
    using key_slot_t = key_slot_type_;

    /** The volume every run of this collection lives on. */
    using volume_t = volume_type_;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The tree the flushed writes of this collection live in. */
    using tree_t = lsm_tree<key_slot_t, volume_t>;

    /** One level set of that tree held against publication. */
    using snapshot_t = collection_snapshot<key_slot_t, volume_t>;

    /** The values of recently read keys this collection keeps. */
    using value_cache_t = value_cache<stored_key_t, key_slot_t::value_layout_k>;

    /** Where the sections of one flushed run sit inside its blob. */
    using sorted_run_layout_t = sorted_run_layout<key_slot_t>;

    /** How the values of this collection are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

    /** Whether the values sit in the key slots themselves, so reading one needs no I/O. */
    static constexpr bool holds_values_inline_k = is_inline(value_layout_k);

    /** One staged write as a scan sees it, without the bytes a select would have to read. */
    struct staged_entry_t {

        /** The key the write named. */
        stored_key_t stored_key = stored_key_t {};

        /** Bytes the staged value takes, which is zero for an erasure and for an empty value. */
        stored_value_size_t size = 0;

        /** Whether the write stored a value or erased the key. */
        select_result_t state = select_result_t::nothing_k;
    };

#pragma endregion Types

#pragma region Construction

    collection() noexcept = default;
    collection(collection const&) = delete;
    collection& operator=(collection const&) = delete;
    collection(collection&&) noexcept = default;
    collection& operator=(collection&&) noexcept = default;

    /**
     *  @brief An empty collection over @p volume, whose memory is charged to @p constraint.
     *  @param[inout] volume The volume every run lands on, borrowed and outliving the collection.
     *  @param[in] config The write buffer bounds, the cache bound and the shape of the tree below.
     *  @param[inout] constraint The budget the write buffer and the cache are charged to.
     */
    static expected<collection> mount(volume_t& volume, collection_config_t const& config,
                                      resource_constraint_t& constraint) noexcept {
        sorted_run_config_t const& values = config.lsm_tree.lsm_level_0.sorted_run;
        collection mounted;
        mounted.config_ = config;
        mounted.constraint_ = &constraint;
        mounted.value_size_ = is_fixed(value_layout_k) ? values.value_size : 0;
        mounted.value_max_size_ = is_inline(value_layout_k)
                                      ? static_cast<stored_value_size_t>(value_layout_k)
                                      : (is_fixed(value_layout_k) ? values.value_size : values.value_max_size);
        if (mounted.value_max_size_ == 0)
            return status_t {status_code_t::invalid_argument_k, "A collection admits values of some width"};

        // A staged value is located by a 32-bit offset whose two largest words name an erasure and an
        // empty value, so the write buffer can never grow to where a real offset would read as either.
        mounted.staged_bytes_limit_ = config.write_buffer_max_bytes;
        if (mounted.staged_bytes_limit_ >= empty_stored_value_offset_k)
            mounted.staged_bytes_limit_ = empty_stored_value_offset_k - 1;
        mounted.staged_count_limit_ = config.write_buffer_max_elements;
        if (mounted.staged_count_limit_ == 0)
            return status_t {status_code_t::invalid_argument_k, "A collection stages at least one write"};

        mounted.staged_ = staged_t {st::hash<stored_key_t> {}, st::equal_to_t {},
                                    constrained_allocator<std::byte> {constraint}};
        mounted.arena_ = arena_t {constrained_allocator<byte_t> {constraint}};
        mounted.cache_ = value_cache_t::of(holds_values_inline_k ? 0 : config.cache_limit, mounted.value_max_size_,
                                           constraint);

        expected<tree_t> tree = tree_t::mount(volume, config.lsm_tree);
        if (!tree) return tree.status();
        mounted.tree_ = std::move(*tree);
        return mounted;
    }

    /** Returns every run to the volume and forgets every staged write, which destroys the data. */
    status_t destroy(io_lane_t& lane) noexcept {
        status_t const dropped = tree_.destroy(lane);
        forget_staged_();
        cache_.clear();
        return dropped;
    }

#pragma endregion Construction

#pragma region Shape

    /** The bounds this collection was mounted with. */
    [[nodiscard]] collection_config_t const& config() const noexcept { return config_; }

    /** The tree the flushed writes live in, which a database compacts and exports through. */
    [[nodiscard]] tree_t& tree() noexcept { return tree_; }

    /** The tree the flushed writes live in. */
    [[nodiscard]] tree_t const& tree() const noexcept { return tree_; }

    /** Bytes the largest value this collection admits takes. */
    [[nodiscard]] stored_value_size_t value_max_size() const noexcept { return value_max_size_; }

    /** Writes staged and not yet flushed. */
    [[nodiscard]] std::size_t staged_count() const noexcept { return staged_.size(); }

    /** Whether nothing is staged and no level holds a key. */
    [[nodiscard]] bool empty() const noexcept { return staged_.empty() && tree_.snapshot().population_count() == 0; }

    /** Keys staged plus key slots every level holds, one key counted once per run that holds it. */
    [[nodiscard]] std::uint64_t population_count() const noexcept {
        return staged_.size() + tree_.snapshot().population_count();
    }

    /** Bytes every run of every level takes on its volume. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept { return tree_.snapshot().size_bytes(); }

#pragma endregion Shape

#pragma region Lookups

    /**
     *  @brief Whether anything carries @p stored_key, and how many bytes its value takes.
     *  @param[in] stored_key The key to look for.
     *  @param[out] size Bytes the value takes, which is zero for anything but @c success_k.
     */
    [[nodiscard]] select_result_t find(stored_key_t stored_key, stored_value_size_t& size) const noexcept {
        size = 0;
        staged_place_t staged;
        if (staged_place_(stored_key, staged)) {
            if (staged.state == staged_state_t::erased_k) return select_result_t::erased_k;
            size = staged.size;
            return select_result_t::success_k;
        }
        return snapshot_t::of(tree_).find(stored_key, size);
    }

    /**
     *  @brief Copies the value of @p stored_key into @p into, which needs only the value's own bytes.
     *  @param[inout] lane The lane the read is submitted and reaped on, held by the calling thread.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     *  @return @c not_found_k for a miss or an erasure, @c io_k for a run shorter than its index claims.
     */
    status_t select(io_lane_t& lane, stored_key_t stored_key, std::span<byte_t> into,
                    stored_value_size_t& size) const noexcept {
        size = 0;
        staged_place_t staged;
        if (staged_place_(stored_key, staged)) {
            if (staged.state == staged_state_t::erased_k)
                return {status_code_t::not_found_k, "A staged write erased that key"};
            if (into.size() < staged.size)
                return {status_code_t::invalid_argument_k, "A buffer shorter than the value"};
            std::span<byte_t const> const bytes = staged_bytes_(staged);
            if (!bytes.empty()) std::memcpy(into.data(), bytes.data(), bytes.size());
            size = staged.size;
            return {};
        }

        if constexpr (!holds_values_inline_k) {
            bool refused = false;
            bool const cached = cache_.visit(stored_key, [&](std::span<byte_t const> bytes) noexcept {
                if (into.size() < bytes.size()) {
                    refused = true;
                    return;
                }
                if (!bytes.empty()) std::memcpy(into.data(), bytes.data(), bytes.size());
                size = static_cast<stored_value_size_t>(bytes.size());
            });
            if (refused) return {status_code_t::invalid_argument_k, "A buffer shorter than the value"};
            if (cached) return {};
        }

        if (status_t const read = snapshot_t::of(tree_).select(lane, stored_key, into, size); read.failed())
            return read;
        if constexpr (!holds_values_inline_k) cache_.add(stored_key, std::span<byte_t const> {into.data(), size});
        return {};
    }

    /**
     *  @brief Reads the values of @p stored_keys, refilling the lane as completions retire.
     *  @param[inout] lane The lane the reads are submitted and reaped on, held by the calling thread.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from an erasure.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     *  @return Success, or the failure that stopped the batch rather than one of its keys.
     *
     *  @note A value larger than one block arrives as several calls, each carrying the bytes of the
     *      value before it, and a key whose value is present and empty is reported once with no bytes.
     *  @note The bytes a batched read moves are never cached, since caching one would mean staging the
     *      whole value in a buffer this path exists to do without.
     */
    template <typename sink_type_>
    status_t select(io_lane_t& lane, std::span<stored_key_t const> stored_keys, std::span<select_result_t> results,
                    sink_type_&& sink) const noexcept {
        if (results.size() != stored_keys.size())
            return {status_code_t::invalid_argument_k, "A batched select answers one result per key"};

        // The tree takes its keys contiguous, so the request each deferred key came from travels beside it.
        deferred_keys_t deferred_keys {constrained_allocator<stored_key_t> {*constraint_}};
        deferred_requests_t deferred_requests {constrained_allocator<std::size_t> {*constraint_}};
        if (deferred_keys.reserve(stored_keys.size()) != st::status_t::success_k ||
            deferred_requests.reserve(stored_keys.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A collection could not track the keys of a batch"};

        for (std::size_t request = 0; request != stored_keys.size(); ++request) {
            staged_place_t staged;
            if (staged_place_(stored_keys[request], staged)) {
                if (staged.state == staged_state_t::erased_k) results[request] = select_result_t::erased_k;
                else {
                    results[request] = select_result_t::success_k;
                    sink(request, std::uint64_t {0}, staged_bytes_(staged));
                }
                continue;
            }
            if constexpr (!holds_values_inline_k) {
                bool const cached = cache_.visit(stored_keys[request], [&](std::span<byte_t const> bytes) noexcept {
                    sink(request, std::uint64_t {0}, bytes);
                });
                if (cached) {
                    results[request] = select_result_t::success_k;
                    continue;
                }
            }
            results[request] = select_result_t::nothing_k;
            [[maybe_unused]] st::status_t const named = deferred_keys.push_back(stored_key_t {stored_keys[request]});
            [[maybe_unused]] st::status_t const tracked = deferred_requests.push_back(std::size_t {request});
        }
        if (deferred_keys.empty()) return {};

        deferred_results_t deferred_results {constrained_allocator<select_result_t> {*constraint_}};
        if (deferred_results.resize(deferred_keys.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A collection could not track the keys of a batch"};

        auto relay = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            sink(deferred_requests[request], offset, bytes);
        };
        if (status_t const read = snapshot_t::of(tree_).select(lane, std::span<stored_key_t const> {deferred_keys},
                                                               std::span<select_result_t> {deferred_results}, relay);
            read.failed())
            return read;
        for (std::size_t index = 0; index != deferred_results.size(); ++index)
            results[deferred_requests[index]] = deferred_results[index];
        return {};
    }

#pragma endregion Lookups

#pragma region Writes

    /** Stages @p stored_value under @p stored_key, flushing first when the write buffer is full. */
    status_t insert(io_lane_t& lane, stored_key_t stored_key, std::span<byte_t const> stored_value) noexcept {
        if (!admits_(stored_value.size()))
            return {status_code_t::malformed_k, "This collection admits no value of that size"};
        if (status_t const room = make_room_(lane, 1, stored_value.size()); room.failed()) return room;
        return stage_(stored_key, stored_value, staged_state_t::stored_k);
    }

    /** Stages a tombstone under @p stored_key, flushing first when the write buffer is full. */
    status_t erase(io_lane_t& lane, stored_key_t stored_key) noexcept {
        if (status_t const room = make_room_(lane, 1, 0); room.failed()) return room;
        return stage_(stored_key, std::span<byte_t const> {}, staged_state_t::erased_k);
    }

    /**
     *  @brief Stages one value per key, flushing first when the write buffer cannot hold them all.
     *  @param[inout] lane The lane a flush writes on, held by the calling thread.
     *  @param[in] stored_keys The keys to write, in any order and with repeats taking the last value.
     *  @param[in] stored_values The values back to back, in the order their keys name them.
     *  @param[in] sizes Bytes of every value, one per key, which must add up to @p stored_values.
     */
    status_t insert(io_lane_t& lane, std::span<stored_key_t const> stored_keys, std::span<byte_t const> stored_values,
                    std::span<stored_value_size_t const> sizes) noexcept {
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
        if (status_t const room = make_room_(lane, stored_keys.size(), total); room.failed()) return room;

        std::uint64_t offset = 0;
        for (std::size_t index = 0; index != stored_keys.size(); ++index) {
            std::span<byte_t const> const one = stored_values.subspan(static_cast<std::size_t>(offset), sizes[index]);
            if (status_t const staged = stage_(stored_keys[index], one, staged_state_t::stored_k); staged.failed())
                return staged;
            offset += sizes[index];
        }
        return {};
    }

    /** Stages a tombstone under every key of @p stored_keys, flushing first when they do not fit. */
    status_t erase(io_lane_t& lane, std::span<stored_key_t const> stored_keys) noexcept {
        if (status_t const room = make_room_(lane, stored_keys.size(), 0); room.failed()) return room;
        for (std::size_t index = 0; index != stored_keys.size(); ++index)
            if (status_t const staged = stage_(stored_keys[index], std::span<byte_t const> {},
                                               staged_state_t::erased_k);
                staged.failed())
                return staged;
        return {};
    }

#pragma endregion Writes

#pragma region Maintenance

    /** Writes every staged write as one run of level zero, leaving the write buffer empty. */
    status_t flush(io_lane_t& lane) noexcept {
        if (staged_.empty()) return {};
        drained_slots_t key_slots {constrained_allocator<key_slot_t> {*constraint_}};
        drained_values_t stored_values {constrained_allocator<byte_t> {*constraint_}};
        if (status_t const drained = drain_(key_slots, stored_values); drained.failed()) return drained;
        if (status_t const published = tree_.flush(lane, std::span<key_slot_t const> {key_slots},
                                                   std::span<byte_t const> {stored_values}, value_max_size_);
            published.failed())
            return published;
        forget_staged_();

        // A cached value the flush has just replaced would outrank the run that now carries it.
        cache_.clear();
        return {};
    }

    /** Whether any level of the tree is over the bytes its number admits. */
    [[nodiscard]] bool needs_compaction() const noexcept { return tree_.needs_compaction(); }

    /**
     *  @brief Merges the shallowest level that is over its bounds into the level below it.
     *  @param[inout] reading_lane The lane the merge gathers source values on, held by the calling thread.
     *  @param[inout] writing_lane The lane the merge writes its new runs on, which is not @p reading_lane.
     */
    expected<compaction_outcome_t> compact(io_lane_t& reading_lane, io_lane_t& writing_lane) noexcept {
        return tree_.compact(reading_lane, writing_lane, compaction_placement_t::automatic_k, nullptr);
    }

    /** Flushes the write buffer, then holds the level set the flush published. */
    expected<snapshot_t> take_snapshot(io_lane_t& lane) noexcept {
        if (status_t const flushed = flush(lane); flushed.failed()) return flushed;
        return snapshot_t::of(tree_);
    }

    /** Lists every staged write in ascending order of its key, which is what a scan merges levels with. */
    status_t staged_in_order(st::basic_vector<staged_entry_t>& into) const noexcept {
        into.clear();
        if (into.reserve(staged_.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A scan could not list the staged writes"};
        [[maybe_unused]] st::status_t const walked = staged_.for_each([&](auto const& slot) noexcept {
            staged_place_t const& place = slot.value();
            staged_entry_t entry;
            entry.stored_key = slot.key();
            entry.size = place.state == staged_state_t::erased_k ? 0 : place.size;
            entry.state = place.state == staged_state_t::erased_k ? select_result_t::erased_k
                                                                  : select_result_t::success_k;
            [[maybe_unused]] st::status_t const listed = into.push_back(std::move(entry));
            return st::walk_control_t::resume_k;
        });
        std::sort(into.begin(), into.end(), [](staged_entry_t const& first, staged_entry_t const& second) noexcept {
            return first.stored_key < second.stored_key;
        });
        return {};
    }

#pragma endregion Maintenance

  private:

#pragma region Private Types

    /** What one staged write did to its key, which the flush turns into a key slot. */
    enum class staged_state_t : std::uint8_t {

        /** The write erased the key, so the flush carries a tombstone into the tree. */
        erased_k,

        /** The write stored bytes, of which there may be none. */
        stored_k,
    };

    /** Where one staged write put its bytes, and whether it erased the key instead. */
    struct staged_place_t {

        /** First byte of the value inside the arena, meaningless for an erasure. */
        std::uint64_t offset = 0;

        /** Bytes the value occupies, which is zero for an empty value and for an erasure. */
        stored_value_size_t size = 0;

        /** Whether the write stored a value or erased the key. */
        staged_state_t state = staged_state_t::stored_k;
    };

    /** The staged writes, addressed by their key, of which the last write of a key wins. */
    using staged_t = st::hash_map<stored_key_t, staged_place_t, st::hash<stored_key_t>, st::equal_to_t,
                                  constrained_allocator<std::byte>>;

    /** The bytes every staged value was copied into, appended to and never rewritten. */
    using arena_t = st::basic_vector<byte_t, constrained_allocator<byte_t>>;

    /** The key slots one flush writes, in ascending order of their keys. */
    using drained_slots_t = st::basic_vector<key_slot_t, constrained_allocator<key_slot_t>>;

    /** The values one flush writes, back to back in the order the key slots locate them. */
    using drained_values_t = st::basic_vector<byte_t, constrained_allocator<byte_t>>;

    /** The keys of a batch that neither the write buffer nor the cache answered. */
    using deferred_keys_t = st::basic_vector<stored_key_t, constrained_allocator<stored_key_t>>;

    /** The caller's request every deferred key came from. */
    using deferred_requests_t = st::basic_vector<std::size_t, constrained_allocator<std::size_t>>;

    /** What the tree answered for every deferred key. */
    using deferred_results_t = st::basic_vector<select_result_t, constrained_allocator<select_result_t>>;

#pragma endregion Private Types

#pragma region Private Methods

    /**
     *  @brief Whether a value of @p size bytes is one this collection's layout can hold.
     *
     *  @note An inline layout takes no empty value, since a key slot holding zero bytes is the
     *      tombstone itself and no width is left to tell the two apart.
     */
    [[nodiscard]] bool admits_(std::size_t size) const noexcept {
        if constexpr (is_fixed(value_layout_k)) return size == value_size_;
        else if constexpr (holds_values_inline_k) return size != 0 && size <= value_max_size_;
        else return size <= value_max_size_;
    }

    /** The staged write under @p stored_key, and whether one is staged at all. */
    [[nodiscard]] bool staged_place_(stored_key_t stored_key, staged_place_t& place) const noexcept {
        bool found = false;
        staged_.probe_to_find(stored_key, [&](auto const& slot) noexcept {
            place = slot.value();
            found = true;
        });
        return found;
    }

    /** The arena bytes @p place names, which is an empty span for an empty value and for an erasure. */
    [[nodiscard]] std::span<byte_t const> staged_bytes_(staged_place_t const& place) const noexcept {
        if (place.size == 0) return {};
        return std::span<byte_t const> {arena_.data() + place.offset, place.size};
    }

    /** Whether @p count more writes of @p bytes together still fit in the write buffer. */
    [[nodiscard]] bool can_stage_(std::size_t count, std::uint64_t bytes) const noexcept {
        return staged_.size() + count <= staged_count_limit_ && arena_used_ + bytes <= staged_bytes_limit_;
    }

    /** Flushes when @p count writes of @p bytes do not fit, and refuses when they never would. */
    status_t make_room_(io_lane_t& lane, std::size_t count, std::uint64_t bytes) noexcept {
        if (can_stage_(count, bytes)) return {};
        if (status_t const flushed = flush(lane); flushed.failed()) return flushed;
        if (can_stage_(count, bytes)) return {};
        return {status_code_t::capacity_exhausted_k, "That batch is larger than the whole write buffer"};
    }

    /** Copies @p stored_value into the arena and names it under @p stored_key. */
    status_t stage_(stored_key_t stored_key, std::span<byte_t const> stored_value, staged_state_t state) noexcept {
        std::uint64_t const wanted = arena_used_ + stored_value.size();
        if (arena_.size() < wanted && arena_.resize(static_cast<std::size_t>(wanted)) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A write buffer could not hold another value"};
        if (staged_.reserve(staged_.size() + 1) == staged_t::reserve_result_t::failed_k)
            return {status_code_t::out_of_memory_k, "A write buffer could not name another key"};

        staged_place_t place;
        place.offset = arena_used_;
        place.size = static_cast<stored_value_size_t>(stored_value.size());
        place.state = state;
        if (!stored_value.empty()) std::memcpy(arena_.data() + arena_used_, stored_value.data(), stored_value.size());
        staged_.emplace(stored_key, staged_place_t {place}, st::assume_reserved_t {});
        arena_used_ += stored_value.size();
        return {};
    }

    /** Empties the write buffer, which both a flush and a destruction do. */
    void forget_staged_() noexcept {
        staged_.clear();
        arena_used_ = 0;
    }

    /**
     *  @brief Lays the staged writes out as one run's key slots and the bytes they locate.
     *  @param[out] key_slots The key slots in ascending order of their keys, as a run takes them.
     *  @param[out] stored_values The values back to back, empty for a layout holding them inline.
     *
     *  @note Each slot carries its arena offset until the values are packed, so the order is found
     *      once by sorting the slots themselves and no permutation is ever built.
     */
    status_t drain_(drained_slots_t& key_slots, drained_values_t& stored_values) const noexcept {
        if (key_slots.reserve(staged_.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A flush could not hold its key slots"};
        [[maybe_unused]] st::status_t const walked = staged_.for_each([&](auto const& slot) noexcept {
            staged_place_t const& place = slot.value();
            key_slot_t built {};
            built.stored_key = slot.key();
            if constexpr (holds_values_inline_k) {
                if (place.state == staged_state_t::stored_k) {
                    built.size = place.size;
                    if (place.size != 0) std::memcpy(built.data, arena_.data() + place.offset, place.size);
                }
            }
            else {
                if (place.state == staged_state_t::erased_k) built.make_tombstone();
                else {
                    built.size = place.size;
                    built.location = static_cast<stored_value_offset_t>(place.offset);
                }
            }
            [[maybe_unused]] st::status_t const placed = key_slots.push_back(std::move(built));
            return st::walk_control_t::resume_k;
        });
        std::sort(key_slots.begin(), key_slots.end());

        if constexpr (!holds_values_inline_k) {
            std::uint64_t total = 0;
            for (std::size_t index = 0; index != key_slots.size(); ++index)
                if (!key_slots[index].is_tombstone()) total += key_slots[index].size;
            if (stored_values.resize(static_cast<std::size_t>(total)) != st::status_t::success_k)
                return {status_code_t::out_of_memory_k, "A flush could not hold its values"};

            std::uint64_t location = sorted_run_layout_t::values_offset(key_slots.size());
            std::uint64_t written = 0;
            for (std::size_t index = 0; index != key_slots.size(); ++index) {
                key_slot_t& built = key_slots[index];
                if (built.is_tombstone()) continue;
                if (built.size == 0) {
                    built.location = empty_stored_value_offset_k;
                    continue;
                }
                std::memcpy(stored_values.data() + written, arena_.data() + built.location, built.size);
                built.location = static_cast<stored_value_offset_t>(location);
                location += built.size;
                written += built.size;
            }
        }
        return {};
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The bounds this collection was mounted with. */
    collection_config_t config_ {};

    /** The budget the write buffer and the cache are charged to, null until the collection is mounted. */
    resource_constraint_t* constraint_ = nullptr;

    /** The tree the flushed writes live in. */
    tree_t tree_;

    /** The staged writes, addressed by their key. */
    staged_t staged_;

    /** The bytes every staged value was copied into. */
    arena_t arena_;

    /** Bytes of the arena the staged writes have taken. */
    std::uint64_t arena_used_ = 0;

    /** The values of recently read keys, filled by point reads and emptied by a flush. */
    value_cache_t mutable cache_;

    /** Bytes every value takes, zero for a layout whose values differ in width. */
    stored_value_size_t value_size_ = 0;

    /** Bytes the largest value this collection admits takes. */
    stored_value_size_t value_max_size_ = 0;

    /** Writes the write buffer holds at once. */
    std::size_t staged_count_limit_ = 0;

    /** Bytes the write buffer holds at once. */
    std::uint64_t staged_bytes_limit_ = 0;

#pragma endregion Private Members
};

#pragma endregion Collection

} // namespace unum::ustore::nvme
