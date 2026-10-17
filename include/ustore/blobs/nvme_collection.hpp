/**
 *  @file include/ustore/blobs/nvme_collection.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief One collection: a write buffer and a value cache in front of an LSM tree of sorted runs, the snapshots
 *      every read runs against, and the ordered walk over any value layout.
 *
 *  Three places can answer a key, and they are consulted newest first: the write buffer holds what has
 *  not been flushed, the value cache holds what a point read already paid for, and the tree holds the
 *  rest. Only the third moves bytes, and it moves them through one snapshot of the level set, so a
 *  batched select reaches the queue pair's depth rather than the size of whatever batch it was handed.
 *  Nothing here allocates an I/O buffer. A caller's span is never registered memory, so the bytes are
 *  copied out of the channel's own blocks either way, and that one copy is the whole of the read path.
 *
 *  A value cache hit is served as a view of the bytes the cache already owns rather than as a copy into a buffer
 *  the caller had to size first, so a batched read that finds a key there costs one hash probe and no
 *  allocation at all. The bound is a count rather than a byte total, derived once from the budget and
 *  the width the collection's values take.
 *
 *  Taking a snapshot copies no run and moves no byte: it counts a reader into the live level set, which
 *  a later publication replaces beside rather than in place, so the runs a snapshot names stay
 *  readable until it is let go. What a snapshot cannot see is the write buffer, which is why a caller
 *  taking one flushes first. Every read of an LSM tree lives in the snapshot and nowhere else.
 *
 *  A run answers a key slot by sorted position, so an ordered walk is a merge over one cursor per run and one
 *  over the staged writes, and nothing is materialized but the staged keys, which the write buffer's
 *  own bound already caps. Where several runs carry the same key the shallowest level wins, and inside
 *  a level that may overlap the youngest run does; the staged writes outrank every run. There is no prefetcher,
 *  and adding one would be a mistake rather than an optimization: a scan that wants bytes hands the next keys to
 *  @c collection::select, which already refills the ring as completions retire, while a second pipeline reading
 *  ahead on the same channel would poll the first one's completions and drop them.
 */

#pragma once

#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm> // `std::min`, `std::sort`
#include <array>     // `std::array`
#include <atomic>    // `std::memory_order_acquire`
#include <bit>       // `std::bit_floor`
#include <span>      // `std::span`
#include <utility>   // `std::exchange`, `std::move`, `std::swap`

#include <forkunion/atomics.hpp>      // `fu::preferred_atomic_ref`
#include <forkunion/capabilities.hpp> // `fu::preferred_yield_t`

#include <smashtable/basic_hash_table.hpp> // `st::hash_map`, `st::assume_reserved`
#include <smashtable/basic_ring.hpp>       // `st::basic_ring`, `st::ring_eviction_t`
#include <smashtable/basic_vector.hpp>     // `st::basic_vector`

#include "ustore/blobs/nvme_key_slot.hpp"     // `limit`, `stored_value_size_t`, `value_layout_t`
#include "ustore/blobs/nvme_tree.hpp"         // `lsm_tree`, `lsm_tree_position_t`, `collection_config_t`
#include "ustore/utilities/nvme_channels.hpp" // `io_channel_t`
#include "ustore/utilities/nvme_ring.hpp"     // `resource_constraint_t`, `constrained_allocator`
#include "ustore/utilities/types.hpp"         // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::blobs {

using utilities::constrained_allocator;
using utilities::constrained_buffer_t;
using utilities::resource_constraint_t;

#pragma region Value Cache

#pragma region Cache

/**
 *  @brief Values of recently read keys, bounded by a count and evicted oldest first.
 *
 *  One collection owns one cache, whose bytes are charged to the budget the cache was made with. The
 *  budget is borrowed and must outlive the cache.
 */
template <typename stored_key_type_, value_layout_t value_layout_>
class value_cache {
  public:

#pragma region Types

    /** The key the cache addresses its values by. */
    using stored_key_t = stored_key_type_;

    /** The layout this cache serves, which fixes how its capacity was derived. */
    static constexpr value_layout_t value_layout_k = value_layout_;

#pragma endregion Types

#pragma region Construction

    value_cache() noexcept = default;
    value_cache(value_cache const&) = delete;
    value_cache& operator=(value_cache const&) = delete;

    value_cache(value_cache&& other) noexcept
        : constraint_(std::exchange(other.constraint_, nullptr)), cached_(std::move(other.cached_)),
          oldest_first_(std::move(other.oldest_first_)), capacity_(std::exchange(other.capacity_, 0)) {}

    value_cache& operator=(value_cache&& other) noexcept {
        if (this == &other) return *this;
        std::swap(constraint_, other.constraint_);
        std::swap(cached_, other.cached_);
        std::swap(oldest_first_, other.oldest_first_);
        std::swap(capacity_, other.capacity_);
        return *this;
    }

    /**
     *  @brief A cache holding what @p capacity_bytes of @p constraint afford of values @p value_size wide.
     *  @param[in] capacity_bytes Bytes the cache may take, where zero leaves it holding nothing.
     *  @param[in] value_size Bytes one value takes, which is its widest for a variable-width layout.
     *  @param[inout] constraint The budget the cached bytes are charged to, which must outlive the cache.
     */
    [[nodiscard]] static value_cache of(std::size_t capacity_bytes, stored_value_size_t value_size,
                                        resource_constraint_t& constraint) noexcept {
        value_cache cache;
        cache.constraint_ = &constraint;
        cache.cached_ = cached_t {stored_key_hash_t {}, st::equal_to_t {},
                                  constrained_allocator<std::byte> {constraint}};
        if (capacity_bytes == 0 || value_size == 0) return cache;

        std::size_t const bytes_per_element = bytes_per_element_k + value_size;
        std::size_t const affordable = std::min(capacity_bytes / bytes_per_element, oldest_first_t::capacity_limit_k);
        if (affordable == 0) return cache;

        // The eviction ring takes a power-of-two count of keys and refuses any other, so the budget
        // is rounded down to one rather than silently leaving the cache holding nothing.
        cache.capacity_ = std::bit_floor(affordable);
        auto oldest_first = oldest_first_t::make(cache.capacity_, constrained_allocator<stored_key_t> {constraint});
        if (!oldest_first) {
            cache.capacity_ = 0;
            return cache;
        }
        cache.oldest_first_ = std::move(*oldest_first);
        if (cache.cached_.reserve(cache.capacity_) == cached_t::reserve_result_t::failed_k) cache.capacity_ = 0;
        return cache;
    }

#pragma endregion Construction

#pragma region Lookups

    /** The entries the cache holds at once, zero when it caches nothing. */
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    /** The entries the cache holds right now. */
    [[nodiscard]] std::size_t size() const noexcept { return cached_.size(); }

    /**
     *  @brief Calls @p visitor with the bytes cached under @p stored_key, which the cache still owns.
     *  @return Whether the key was cached, which is the only case the visitor is called in.
     */
    template <typename visitor_type_>
    [[nodiscard]] bool visit(stored_key_t stored_key, visitor_type_&& visitor) const noexcept {
        bool seen = false;
        cached_.probe_to_find(stored_key, [&](auto const& slot) noexcept {
            constrained_buffer_t const& buffer = slot.value();
            visitor(std::span<byte_t const> {buffer});
            seen = true;
        });
        return seen;
    }

    /** Caches @p stored_value under @p stored_key, evicting the oldest entry once the bound is reached. */
    void add(stored_key_t stored_key, std::span<byte_t const> stored_value) noexcept {
        if (capacity_ == 0) return;

        constrained_buffer_t buffer {constrained_allocator<std::byte> {*constraint_}};
        if (st::failed(buffer.resize(stored_value.size()))) return;
        if (!stored_value.empty()) std::memcpy(buffer.data(), stored_value.data(), stored_value.size());

        bool replaced = false;
        cached_.probe_to_find(stored_key, [&](auto& slot) noexcept {
            slot.value_ref() = std::move(buffer);
            replaced = true;
        });
        if (replaced) return;

        stored_key_t evicted {};
        if (oldest_first_.push_evicting(stored_key_t {stored_key}, evicted) == st::ring_eviction_t::evicted_oldest_k) {
            [[maybe_unused]] bool const dropped = cached_.erase(evicted);
        }
        cached_.emplace(stored_key, std::move(buffer), st::assume_reserved);
    }

    /** Drops every cached value, which a flush does because a staged write has replaced them. */
    void clear() noexcept {
        cached_.clear();
        oldest_first_.clear();
    }

#pragma endregion Lookups

  private:

#pragma region Private Types

    /** The cached values, addressed by their stored key. */
    using cached_t = st::hash_map<stored_key_t, constrained_buffer_t, stored_key_hash_t, st::equal_to_t,
                                  constrained_allocator<std::byte>>;

    /** The cached keys in the order they arrived. */
    using oldest_first_t = st::basic_ring<stored_key_t, constrained_allocator<stored_key_t>>;

#pragma endregion Private Types

#pragma region Private Members

    /** Bytes one entry takes beyond its value: the key twice, in the table and in the ring, and the buffer header. */
    static constexpr std::size_t bytes_per_element_k = 2 * sizeof(stored_key_t) + sizeof(constrained_buffer_t);

    /** The budget every cached byte is charged to, null until the cache is made. */
    resource_constraint_t* constraint_ = nullptr;

    /** The cached values, addressed by their stored key. */
    cached_t cached_;

    /** The cached keys in the order they arrived, whose oldest an insertion at the bound evicts. */
    oldest_first_t oldest_first_;

    /** The entries the cache holds at once, zero when it caches nothing. */
    std::size_t capacity_ = 0;

#pragma endregion Private Members
};

#pragma endregion Cache

#pragma endregion Value Cache

#pragma region Collection Snapshot

#pragma region Vocabulary

/** What one key of a select answered, which separates a miss from an erasure and from a truncated run. */
enum class select_result_t : std::uint8_t {

    /** Neither the write buffer nor any level carries that key. */
    nothing_k,

    /** A tombstone carries that key, so its value has been erased. */
    erased_k,

    /** Every byte the key slot claims arrived. */
    success_k,

    /** The medium moved fewer bytes than the key slot claims, so this one key failed. */
    incomplete_k,
};

#pragma endregion Vocabulary

#pragma region Snapshot

/**
 *  @brief One level set held against publication, and the point and batched reads that run against it.
 *
 *  A snapshot borrows the tree it counted itself into, so the tree must outlive it. It is move-only
 *  because the count it holds belongs to exactly one owner.
 */
template <typename key_slot_type_, typename volume_type_>
class collection_snapshot {
  public:

#pragma region Types

    /** The key slot every run of the held levels holds. */
    using key_slot_t = key_slot_type_;

    /** The volume every run of the held levels lives on. */
    using volume_t = volume_type_;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The tree this snapshot holds a level set of. */
    using tree_t = lsm_tree<key_slot_t, volume_t>;

    /** The hold itself, which keeps the level set alive for as long as this snapshot lives. */
    using levels_t = typename tree_t::snapshot_t;

    /** How the values of this collection are stored. */
    static constexpr value_layout_t value_layout_k = key_slot_t::value_layout_k;

    /** Whether the values sit in the key slots themselves, so reading one needs no I/O. */
    static constexpr bool holds_values_inline_k = is_inline(value_layout_k);

#pragma endregion Types

#pragma region Construction

    collection_snapshot() noexcept = default;
    collection_snapshot(collection_snapshot const&) = delete;
    collection_snapshot& operator=(collection_snapshot const&) = delete;

    collection_snapshot(collection_snapshot&& other) noexcept
        : tree_(std::exchange(other.tree_, nullptr)), levels_(std::move(other.levels_)) {}

    collection_snapshot& operator=(collection_snapshot&& other) noexcept {
        if (this == &other) return *this;
        tree_ = std::exchange(other.tree_, nullptr);
        levels_ = std::move(other.levels_);
        return *this;
    }

    /** Holds the live level set of @p tree, which must outlive the snapshot. */
    [[nodiscard]] static collection_snapshot of(tree_t const& tree) noexcept {
        collection_snapshot taken;
        taken.tree_ = &tree;
        taken.levels_ = tree.snapshot();
        return taken;
    }

#pragma endregion Construction

#pragma region Shape

    /** Whether this snapshot holds a level set. */
    [[nodiscard]] bool holding() const noexcept { return tree_ != nullptr && levels_.holding(); }

    /** The level set held, which a scan walks the runs of. */
    [[nodiscard]] levels_t const& levels() const noexcept { return levels_; }

    /** Key slots every level holds together, one key counted once per run that holds it. */
    [[nodiscard]] std::uint64_t population_count() const noexcept { return levels_.population_count(); }

    /** Bytes every run of every level takes on its volume. */
    [[nodiscard]] std::uint64_t size_bytes() const noexcept { return levels_.size_bytes(); }

#pragma endregion Shape

#pragma region Lookups

    /**
     *  @brief Whether the held levels carry @p stored_key, and how many bytes its value takes.
     *  @param[in] stored_key The key to look for.
     *  @param[out] size Bytes the value takes, which is zero for anything but @c success_k.
     */
    [[nodiscard]] select_result_t find(stored_key_t stored_key, stored_value_size_t& size) const noexcept {
        size = 0;
        lsm_tree_position_t position;
        find_result_t const found = tree_->find(levels_, stored_key, position);
        if (found == find_result_t::nothing_k) return select_result_t::nothing_k;
        if (found == find_result_t::stored_key_k) return select_result_t::erased_k;
        size = static_cast<stored_value_size_t>(
            levels_.level(position.level_number).value_window(position.level_position).bytes);
        return select_result_t::success_k;
    }

    /**
     *  @brief Copies the value of @p stored_key into @p into, which needs only the value's own bytes.
     *  @param[inout] channel The channel the read is submitted and completed on, held by the calling thread.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     *  @return @c not_found_k for a miss or an erasure, @c input_output_k for a run shorter than its index claims.
     */
    status_t select(io_channel_t& channel, stored_key_t stored_key, std::span<byte_t> into,
                    stored_value_size_t& size) const noexcept {
        size = 0;
        stored_value_size_t wanted = 0;
        if (find(stored_key, wanted) != select_result_t::success_k)
            return {status_code_t::not_found_k, "No level of this snapshot carries that key"};
        if (into.size() < wanted) return {status_code_t::invalid_argument_k, "A buffer shorter than the value"};

        std::uint64_t delivered = 0;
        auto sink = [&](std::size_t, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            if (!bytes.empty()) std::memcpy(into.data() + offset, bytes.data(), bytes.size());
            delivered += bytes.size();
        };
        std::array<stored_key_t, 1> const keys {stored_key};
        std::array<find_result_t, 1> answers {find_result_t::nothing_k};
        if (status_t const read = tree_->read_values(channel, levels_, std::span<stored_key_t const> {keys},
                                                     std::span<find_result_t> {answers}, sink);
            read.failed())
            return read;

        // One key has no rest of a batch to keep, so a block short of its last one fails the whole call.
        if (delivered != wanted) return {status_code_t::io_k, "A run is shorter than its index claims"};
        size = wanted;
        return {};
    }

    /**
     *  @brief Reads the values of @p stored_keys, refilling the channel as completions retire.
     *  @param[inout] channel The channel the reads are submitted and completed on, held by the calling thread.
     *  @param[in] stored_keys The keys to read, in any order and with any number of misses among them.
     *  @param[out] results What every key answered, which separates a miss from an erasure.
     *  @param[in] sink Called as @c sink(request, offset, bytes) for every key that locates a value.
     *  @return Success, or the failure that stopped the batch rather than one of its keys.
     *
     *  @note A value larger than one block arrives as several calls, each carrying the bytes of the
     *      value before it, and a key whose value is present and empty is reported once with no bytes.
     */
    template <typename sink_type_>
    status_t select(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<select_result_t> results, sink_type_&& sink) const noexcept {
        if (results.size() != stored_keys.size())
            return {status_code_t::invalid_argument_k, "A batched select answers one result per key"};

        // The tree answers in its own vocabulary and the sink is told nothing about sizes, so the
        // bytes every key owed are counted here and compared once the read has run.
        st::basic_vector<find_result_t> answers;
        st::basic_vector<std::uint64_t> delivered;
        if (answers.resize(stored_keys.size()) != st::status_t::success_k ||
            delivered.resize(stored_keys.size()) != st::status_t::success_k)
            return {status_code_t::out_of_memory_k, "A snapshot could not track the keys of a batch"};
        for (std::size_t request = 0; request != stored_keys.size(); ++request) {
            answers[request] = find_result_t::nothing_k;
            delivered[request] = 0;
        }

        auto relay = [&](std::size_t request, std::uint64_t offset, std::span<byte_t const> bytes) noexcept {
            delivered[request] += bytes.size();
            sink(request, offset, bytes);
        };
        status_t const read = tree_->read_values(channel, levels_, stored_keys, std::span<find_result_t> {answers},
                                                 relay);

        std::size_t truncated = 0;
        for (std::size_t request = 0; request != stored_keys.size(); ++request) {
            if (answers[request] == find_result_t::nothing_k) {
                results[request] = select_result_t::nothing_k;
                continue;
            }
            if (answers[request] == find_result_t::stored_key_k) {
                results[request] = select_result_t::erased_k;
                continue;
            }
            stored_value_size_t wanted = 0;
            [[maybe_unused]] select_result_t const carried = find(stored_keys[request], wanted);
            if (delivered[request] == wanted) results[request] = select_result_t::success_k;
            else {
                results[request] = select_result_t::incomplete_k;
                ++truncated;
            }
        }

        // A block short of what it asked for fails its own key; a cancelled or refused one fails the batch.
        if (read.failed() && (truncated == 0 || !(read == status_code_t::io_k))) return read;
        return {};
    }

#pragma endregion Lookups

  private:

#pragma region Private Members

    /** The tree the held level set belongs to, borrowed and outliving the snapshot. */
    tree_t const* tree_ = nullptr;

    /** The hold itself, released when the snapshot dies. */
    levels_t levels_;

#pragma endregion Private Members
};

#pragma endregion Snapshot

#pragma endregion Collection Snapshot

#pragma region Collection

#pragma region Collection

/**
 *  @brief A write buffer and a value cache in front of an LSM tree of sorted runs.
 *
 *  The volume, the budget and every channel are borrowed and must outlive the collection. A channel belongs
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

        mounted.staged_ = staged_t {stored_key_hash_t {}, st::equal_to_t {},
                                    constrained_allocator<std::byte> {constraint}};
        mounted.arena_ = arena_t {constrained_allocator<byte_t> {constraint}};
        mounted.cache_ = value_cache_t::of(holds_values_inline_k ? 0 : config.cache_limit, mounted.value_max_size_,
                                           constraint);

        auto [tree, mount_tree_status] = tree_t::mount(volume, config.lsm_tree);
        if (mount_tree_status.failed()) return mount_tree_status;
        mounted.tree_ = std::move(tree);
        return mounted;
    }

    /** Returns every run to the volume and forgets every staged write, which destroys the data. */
    status_t destroy(io_channel_t& channel) noexcept {
        status_t const dropped = tree_.destroy(channel);
        forget_staged_();
        cache_guard_t const held {cache_claim_};
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
     *  @param[inout] channel The channel the read is submitted and completed on, held by the calling thread.
     *  @param[in] stored_key The key to read.
     *  @param[out] into Where the value lands, which need not be aligned and need not be registered.
     *  @param[out] size Bytes written, which is zero for every failure.
     *  @return @c not_found_k for a miss or an erasure, @c input_output_k for a run shorter than its index claims.
     */
    status_t select(io_channel_t& channel, stored_key_t stored_key, std::span<byte_t> into,
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
            cache_guard_t const held {cache_claim_};
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

        if (status_t const read = snapshot_t::of(tree_).select(channel, stored_key, into, size); read.failed())
            return read;
        if constexpr (!holds_values_inline_k) {
            cache_guard_t const held {cache_claim_};
            cache_.add(stored_key, std::span<byte_t const> {into.data(), size});
        }
        return {};
    }

    /**
     *  @brief Reads the values of @p stored_keys, refilling the channel as completions retire.
     *  @param[inout] channel The channel the reads are submitted and completed on, held by the calling thread.
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
    status_t select(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<select_result_t> results, sink_type_&& sink) const noexcept {
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
                cache_guard_t const held {cache_claim_};
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
        if (status_t const read = snapshot_t::of(tree_).select(channel, std::span<stored_key_t const> {deferred_keys},
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
    status_t insert(io_channel_t& channel, stored_key_t stored_key, std::span<byte_t const> stored_value) noexcept {
        if (!admits_(stored_value.size()))
            return {status_code_t::malformed_k, "This collection admits no value of that size"};
        if (status_t const room = make_room_(channel, 1, stored_value.size()); room.failed()) return room;
        return stage_(stored_key, stored_value, staged_state_t::stored_k);
    }

    /** Stages a tombstone under @p stored_key, flushing first when the write buffer is full. */
    status_t erase(io_channel_t& channel, stored_key_t stored_key) noexcept {
        if (status_t const room = make_room_(channel, 1, 0); room.failed()) return room;
        return stage_(stored_key, std::span<byte_t const> {}, staged_state_t::erased_k);
    }

    /**
     *  @brief Stages one value per key, flushing first when the write buffer cannot hold them all.
     *  @param[inout] channel The channel a flush writes on, held by the calling thread.
     *  @param[in] stored_keys The keys to write, in any order and with repeats taking the last value.
     *  @param[in] stored_values The values back to back, in the order their keys name them.
     *  @param[in] sizes Bytes of every value, one per key, which must add up to @p stored_values.
     */
    status_t insert(io_channel_t& channel, std::span<stored_key_t const> stored_keys,
                    std::span<byte_t const> stored_values, std::span<stored_value_size_t const> sizes) noexcept {
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
        if (status_t const room = make_room_(channel, stored_keys.size(), total); room.failed()) return room;

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
    status_t erase(io_channel_t& channel, std::span<stored_key_t const> stored_keys) noexcept {
        if (status_t const room = make_room_(channel, stored_keys.size(), 0); room.failed()) return room;
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
    status_t flush(io_channel_t& channel) noexcept {
        if (staged_.empty()) return {};
        drained_slots_t key_slots {constrained_allocator<key_slot_t> {*constraint_}};
        drained_values_t stored_values {constrained_allocator<byte_t> {*constraint_}};
        if (status_t const drained = drain_(key_slots, stored_values); drained.failed()) return drained;
        if (status_t const published = tree_.flush(channel, std::span<key_slot_t const> {key_slots},
                                                   std::span<byte_t const> {stored_values}, value_max_size_);
            published.failed())
            return published;
        forget_staged_();

        // A cached value the flush has just replaced would outrank the run that now carries it.
        cache_guard_t const held {cache_claim_};
        cache_.clear();
        return {};
    }

    /** Whether any level of the tree is over the bytes its number admits. */
    [[nodiscard]] bool needs_compaction() const noexcept { return tree_.needs_compaction(); }

    /**
     *  @brief Merges the shallowest level that is over its bounds into the level below it.
     *  @param[inout] reading_channel The channel the merge gathers source values on, held by the calling thread.
     *  @param[inout] writing_channel The channel the merge writes its new runs on, which is not @p reading_channel.
     */
    expected<compaction_outcome_t> compact(io_channel_t& reading_channel, io_channel_t& writing_channel) noexcept {
        return tree_.compact(reading_channel, writing_channel, compaction_placement_t::automatic_k, nullptr);
    }

    /** Flushes the write buffer, then holds the level set the flush published. */
    expected<snapshot_t> take_snapshot(io_channel_t& channel) noexcept {
        if (status_t const flushed = flush(channel); flushed.failed()) return flushed;
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
    using staged_t =
        st::hash_map<stored_key_t, staged_place_t, stored_key_hash_t, st::equal_to_t, constrained_allocator<std::byte>>;

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
    status_t make_room_(io_channel_t& channel, std::size_t count, std::uint64_t bytes) noexcept {
        if (can_stage_(count, bytes)) return {};
        if (status_t const flushed = flush(channel); flushed.failed()) return flushed;
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
        staged_.emplace(stored_key, staged_place_t {place}, st::assume_reserved);
        arena_used_ += stored_value.size();
        return {};
    }

    /** Empties the write buffer, which both a flush and a destruction do. */
    void forget_staged_() noexcept {
        staged_.clear();
        arena_used_ = 0;
    }

    /** Holds @c cache_ for the life of the guard, waiting while another reader visits or fills it. */
    class cache_guard_t {
      public:
        explicit cache_guard_t(std::int32_t& claim) noexcept : claim_(claim) {
            fu::preferred_atomic_ref<std::int32_t> const word {claim_};
            std::int32_t expected_free = 0;
            while (
                !word.compare_exchange_weak(expected_free, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
                fu::preferred_yield_t {}(claim_, expected_free, std::size_t {0});
                expected_free = 0;
            }
        }
        cache_guard_t(cache_guard_t const&) = delete;
        cache_guard_t& operator=(cache_guard_t const&) = delete;
        ~cache_guard_t() noexcept {
            fu::preferred_atomic_ref<std::int32_t> {claim_}.store(0, std::memory_order_release);
        }

      private:
        std::int32_t& claim_;
    };

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

    /** Held by whoever visits, fills or empties @c cache_, since readers sharing the collection fill it too. */
    std::int32_t mutable cache_claim_ = 0;

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

#pragma endregion Collection

#pragma region Collection Scanner

#pragma region Scanner

/**
 *  @brief Every key of one collection in ascending order, each with what it is and how wide it is.
 *
 *  The scanner holds the level set it walks for its whole life, so a publication on another thread
 *  waits for it; a scan is taken, walked and dropped rather than kept beside a writer.
 */
template <typename key_slot_type_, typename volume_type_>
class collection_scanner {
  public:

#pragma region Types

    /** The key slot every run of the scanned collection holds. */
    using key_slot_t = key_slot_type_;

    /** The volume every run of the scanned collection lives on. */
    using volume_t = volume_type_;

    /** The key the runs order their key slots by. */
    using stored_key_t = typename key_slot_t::stored_key_t;

    /** The collection this scanner walks. */
    using collection_t = collection<key_slot_t, volume_t>;

    /** One level set of that collection's tree held against publication. */
    using snapshot_t = collection_snapshot<key_slot_t, volume_t>;

    /** One staged write as the merge sees it. */
    using staged_entry_t = typename collection_t::staged_entry_t;

#pragma endregion Types

#pragma region Construction

    collection_scanner() noexcept = default;
    collection_scanner(collection_scanner const&) = delete;
    collection_scanner& operator=(collection_scanner const&) = delete;
    collection_scanner(collection_scanner&&) noexcept = default;
    collection_scanner& operator=(collection_scanner&&) noexcept = default;

    /** A scan of @p source positioned on its first key not below @p from, where @p source must outlive the scanner. */
    static expected<collection_scanner> over(collection_t const& source,
                                             stored_key_t from = limit<stored_key_t>::min()) noexcept {
        collection_scanner scanner;
        if (status_t const listed = source.staged_in_order(scanner.staged_); listed.failed()) return listed;
        while (scanner.staged_at_ != scanner.staged_.size() && scanner.staged_[scanner.staged_at_].stored_key < from)
            ++scanner.staged_at_;
        scanner.levels_ = snapshot_t::of(source.tree());

        std::size_t runs = 0;
        for (std::size_t number = 0; number != scanner.levels_.levels().size(); ++number)
            runs += scanner.levels_.levels().level(number).size();
        if (scanner.cursors_.reserve(runs) != st::status_t::success_k)
            return status_t {status_code_t::out_of_memory_k, "A scan could not hold a cursor per run"};

        for (std::size_t number = 0; number != scanner.levels_.levels().size(); ++number) {
            auto const& level = scanner.levels_.levels().level(number);
            for (std::size_t index = 0; index != level.size(); ++index) {
                run_cursor_t cursor;
                cursor.level_number = static_cast<std::uint32_t>(number);
                cursor.run_index = static_cast<std::uint32_t>(index);
                cursor.population = level.run(index).population_count();
                cursor.position = lower_bound_(level.run(index), cursor.population, from);
                [[maybe_unused]] st::status_t const placed = scanner.cursors_.push_back(std::move(cursor));
            }
        }
        scanner.advance();
        return scanner;
    }

#pragma endregion Construction

#pragma region Walk

    /** Whether every key has been visited. */
    [[nodiscard]] bool reached_end() const noexcept { return state_ == select_result_t::nothing_k; }

    /** The key the scan sits on, meaningless once the walk has ended. */
    [[nodiscard]] stored_key_t stored_key() const noexcept { return stored_key_; }

    /** Whether the key the scan sits on carries a value or a tombstone. */
    [[nodiscard]] select_result_t state() const noexcept { return state_; }

    /** Bytes the value of that key takes, which is zero for a tombstone and for an empty value. */
    [[nodiscard]] stored_value_size_t size() const noexcept { return size_; }

    /** Moves to the next key, taking the newest version of it and skipping the versions behind it. */
    void advance() noexcept {
        stored_key_t smallest = limit<stored_key_t>::max();
        bool any = false;
        if (staged_at_ != staged_.size()) {
            smallest = staged_[staged_at_].stored_key;
            any = true;
        }
        for (std::size_t index = 0; index != cursors_.size(); ++index) {
            if (cursors_[index].position == cursors_[index].population) continue;
            stored_key_t const head = key_slot_at_(cursors_[index]).stored_key;
            if (any && !(head < smallest)) continue;
            smallest = head;
            any = true;
        }
        if (!any) {
            state_ = select_result_t::nothing_k;
            size_ = 0;
            return;
        }

        stored_key_ = smallest;
        if (staged_at_ != staged_.size() && staged_[staged_at_].stored_key == smallest) take_staged_();
        else take_youngest_run_(smallest);

        if (staged_at_ != staged_.size() && staged_[staged_at_].stored_key == smallest) ++staged_at_;
        for (std::size_t index = 0; index != cursors_.size(); ++index)
            if (cursors_[index].position != cursors_[index].population &&
                key_slot_at_(cursors_[index]).stored_key == smallest)
                ++cursors_[index].position;
    }

#pragma endregion Walk

  private:

#pragma region Private Types

    /** Where one run of one level the scan walks has got to. */
    struct run_cursor_t {

        /** The level the run belongs to, zero being the one flushes land on. */
        std::uint32_t level_number = 0;

        /** The run inside that level, older first where the runs overlap. */
        std::uint32_t run_index = 0;

        /** The next sorted position inside that run. */
        std::uint64_t position = 0;

        /** Key slots the run holds, past which the cursor is spent. */
        std::uint64_t population = 0;
    };

#pragma endregion Private Types

#pragma region Private Methods

    /** The first of the @p population sorted positions of @p run whose key is not below @p from. */
    template <typename run_type_>
    [[nodiscard]] static std::uint64_t lower_bound_(run_type_ const& run, std::uint64_t population,
                                                    stored_key_t from) noexcept {
        std::uint64_t low = 0;
        std::uint64_t high = population;
        while (low != high) {
            std::uint64_t const middle = low + (high - low) / 2;
            if (run.key_slot(middle).stored_key < from) low = middle + 1;
            else high = middle;
        }
        return low;
    }

    /** The key slot @p cursor sits on, which must not be spent. */
    [[nodiscard]] key_slot_t const& key_slot_at_(run_cursor_t const& cursor) const noexcept {
        return levels_.levels().level(cursor.level_number).run(cursor.run_index).key_slot(cursor.position);
    }

    /** Takes the staged write as the answer, which outranks every run that carries the same key. */
    void take_staged_() noexcept {
        state_ = staged_[staged_at_].state;
        size_ = staged_[staged_at_].size;
    }

    /** Takes the shallowest and, inside a level, the youngest run carrying @p smallest as the answer. */
    void take_youngest_run_(stored_key_t smallest) noexcept {
        std::size_t chosen = cursors_.size();
        for (std::size_t index = 0; index != cursors_.size(); ++index) {
            if (cursors_[index].position == cursors_[index].population) continue;
            if (!(key_slot_at_(cursors_[index]).stored_key == smallest)) continue;
            if (chosen != cursors_.size() && !outranks_(cursors_[index], cursors_[chosen])) continue;
            chosen = index;
        }
        key_slot_t const& slot = key_slot_at_(cursors_[chosen]);
        state_ = slot.is_tombstone() ? select_result_t::erased_k : select_result_t::success_k;
        size_ = slot.is_tombstone() ? 0 : slot.size;
    }

    /** Whether @p first carries a newer version of a key than @p second does. */
    [[nodiscard]] static bool outranks_(run_cursor_t const& first, run_cursor_t const& second) noexcept {
        if (first.level_number != second.level_number) return first.level_number < second.level_number;
        return first.run_index > second.run_index;
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The level set the scan walks, held for the whole life of the scanner. */
    snapshot_t levels_;

    /** Every staged write in ascending order of its key. */
    st::basic_vector<staged_entry_t> staged_;

    /** One cursor per run of every level the snapshot holds. */
    st::basic_vector<run_cursor_t> cursors_;

    /** The staged write the merge would take next. */
    std::size_t staged_at_ = 0;

    /** The key the scan sits on. */
    stored_key_t stored_key_ = stored_key_t {};

    /** What that key carries, which reads as @c nothing_k once the walk has ended. */
    select_result_t state_ = select_result_t::nothing_k;

    /** Bytes the value of that key takes. */
    stored_value_size_t size_ = 0;

#pragma endregion Private Members
};

#pragma endregion Scanner

#pragma endregion Collection Scanner

} // namespace unum::ustore::blobs
