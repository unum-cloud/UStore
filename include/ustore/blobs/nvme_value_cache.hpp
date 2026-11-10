/**
 *  @file include/ustore/blobs/nvme_value_cache.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief The value cache of one store: slots a point read refills in place, probed by one hash and
 *      copied under a sequence check.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstring> // `std::memcpy`, `std::memset`

#include <atomic>  // `std::atomic_thread_fence`, `std::memory_order_acquire`
#include <bit>     // `std::bit_floor`
#include <span>    // `std::span`
#include <utility> // `std::swap`

#include <smashtable/basic_vector.hpp> // `st::vector`
#include <smashtable/shared.hpp>       // `st::status_t`

#include "ustore/blobs/nvme_key_slot.hpp"   // `stored_value_size_t`
#include "ustore/utilities/ram_buffers.hpp" // `constrained_allocator`, `resource_constraint`
#include "ustore/utilities/types.hpp"       // `byte_t`

namespace unum::ustore::blobs {

using utilities::constrained_allocator;
using utilities::resource_constraint;

#pragma region Value Cache

#pragma region Cache

/**
 *  @brief Values of versions point reads already paid I/O for, keyed by the key and the stamp of
 *      the version, so a flush or a compaction moving a version leaves its entry right and nothing
 *      is ever cleared.
 *
 *  A fixed array of slots, a slot per hash of the version, each owning its words from its first
 *  fill until the cache dies and refilled in place. A fill moves the slot's sequence to odd, writes
 *  the version, and moves the sequence on to the next even value; a probe copies the version out
 *  between two loads of the sequence and keeps the copy only when both read the same even value.
 *  Neither ever waits, a probe writes nothing, and a displaced version is simply overwritten, so
 *  nothing is freed until the cache is. Every word moves atomically, so a probe racing a fill
 *  copies a torn version it then drops rather than racing on memory. The words are charged to the
 *  budget the cache was made with, which is borrowed and must outlive the cache.
 *
 *  @tparam atomic_reference_ The reference every word of a slot and the budget are owned through.
 */
template <typename stored_key_type_, template <typename> typename atomic_reference_>
class value_cache {
  public:

#pragma region Types

    /** The key the cache addresses its values by. */
    using stored_key_t = stored_key_type_;

#pragma endregion Types

#pragma region Construction

    value_cache() noexcept = default;
    value_cache(value_cache const&) = delete;
    value_cache& operator=(value_cache const&) = delete;
    value_cache(value_cache&& other) noexcept = default;

    value_cache& operator=(value_cache&& other) noexcept {
        if (this == &other) return *this;
        std::swap(slots_, other.slots_);
        std::swap(slot_words_, other.slot_words_);
        return *this;
    }

    /** Frees the words of every slot filled so far, which only a cache no reader reaches may. */
    ~value_cache() noexcept {
        words_allocator_t allocator {slots_.get_allocator()};
        for (std::uint64_t* words : slots_)
            if (words) allocator.deallocate(words, slot_words_);
    }

    /**
     *  @brief A cache holding what @p capacity_bytes of @p constraint afford of values
     *      @p value_size wide.
     *  @param[in] capacity_bytes Bytes the cache may take, where zero leaves it holding nothing.
     *  @param[in] value_size Bytes one value takes, which is its widest for a
     *      variable-width layout.
     *  @param[in,out] constraint The budget the cached bytes are charged to, which must
     *      outlive the cache.
     */
    [[nodiscard]] static value_cache of(std::size_t capacity_bytes, stored_value_size_t value_size,
                                        resource_constraint<atomic_reference_>& constraint) noexcept {
        value_cache cache;
        cache.slots_ = slots_t {constrained_allocator<std::uint64_t*, atomic_reference_> {constraint}};
        if (capacity_bytes == 0 || value_size == 0) return cache;
        std::size_t const slot_words = value_word_k + words_of_(value_size);
        std::size_t const affordable = capacity_bytes / (slot_words * sizeof(std::uint64_t) + sizeof(std::uint64_t*));
        if (affordable == 0) return cache;
        // A slot is found by masking the hash, so the budget is rounded down to a power of
        // two of slots.
        if (cache.slots_.resize(std::bit_floor(affordable)) != st::status_t::success_k) return cache;
        for (std::uint64_t*& slot : cache.slots_) slot = nullptr;
        cache.slot_words_ = slot_words;
        return cache;
    }

#pragma endregion Construction

#pragma region Lookups

    /** The entries the cache holds at once, zero when it caches nothing. */
    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

    /**
     *  @brief Copies the value cached for the version of @p stored_key stamped @p stamp into
     *      @p into, when it fits.
     *  @param[in] stored_key The key whose cached value is wanted.
     *  @param[in] stamp The stamp of the version wanted.
     *  @param[out] into Room for the value's bytes.
     *  @param[out] size Bytes copied, untouched unless the copy is kept.
     *  @return Whether the slot held that version from the first load of its sequence to the last,
     *      so the copy is kept.
     */
    [[nodiscard]] bool copy(stored_key_t const& stored_key, transaction_stamp_t stamp, std::span<byte_t> into,
                            stored_value_size_t& size) const noexcept {
        if (slots_.empty()) return false;
        std::uint64_t const* const words =
            atomic_reference_<std::uint64_t* const> {slots_[slot_of_(stored_key, stamp)]}.load(
                std::memory_order_acquire);
        if (!words) return false;
        std::uint64_t const sequence = load_(words[sequence_word_k], std::memory_order_acquire);
        // Zero is a slot no fill finished yet, and an odd sequence one a fill is writing.
        if (sequence == 0 || sequence % 2 != 0 || load_(words[stamp_word_k]) != stamp) return false;
        stored_key_t cached {};
        load_bytes_(words + key_word_k, std::as_writable_bytes(std::span<stored_key_t, 1> {&cached, 1}));
        std::uint64_t const cached_size = load_(words[size_word_k]);
        if (!(cached == stored_key) || cached_size > into.size()) return false;
        load_bytes_(words + value_word_k, into.first(static_cast<std::size_t>(cached_size)));
        // Orders every load above before the sequence is read again, as a fill's release fence
        // orders its stores.
        std::atomic_thread_fence(std::memory_order_acquire);
        if (load_(words[sequence_word_k]) != sequence) return false;
        size = static_cast<stored_value_size_t>(cached_size);
        return true;
    }

    /** Caches @p stored_value as the version of @p stored_key stamped @p stamp in the slot that
     *  version hashes to, overwriting whatever it held; a fill finding another fill in the slot, or
     *  the budget refusing the slot's first words, caches nothing. */
    void add(stored_key_t const& stored_key, transaction_stamp_t stamp, std::span<byte_t const> stored_value) noexcept {
        if (slots_.empty() || words_of_(stored_value.size()) > slot_words_ - value_word_k) return;
        std::uint64_t* const words = words_of_slot_(slots_[slot_of_(stored_key, stamp)]);
        if (!words) return;
        atomic_reference_<std::uint64_t> const sequence {words[sequence_word_k]};
        std::uint64_t stable = sequence.load(std::memory_order_relaxed);
        if (stable % 2 != 0 ||
            !sequence.compare_exchange_strong(stable, stable + 1, std::memory_order_relaxed, std::memory_order_relaxed))
            return;
        // Orders the odd sequence before every store below, which a probe's acquire
        // fence pairs with.
        std::atomic_thread_fence(std::memory_order_release);
        store_(words[stamp_word_k], stamp);
        store_(words[size_word_k], stored_value.size());
        store_bytes_(words + key_word_k, std::as_bytes(std::span<stored_key_t const, 1> {&stored_key, 1}));
        store_bytes_(words + value_word_k, stored_value);
        sequence.store(stable + 2, std::memory_order_release);
    }

#pragma endregion Lookups

  private:

#pragma region Private Types

    /** One pointer per slot to the words it owns, null until its first fill, charged
     *  to the budget. */
    using slots_t = st::vector<std::uint64_t*, constrained_allocator<std::uint64_t*, atomic_reference_>>;

    /** Allocates the words of a slot from the same budget. */
    using words_allocator_t = constrained_allocator<std::uint64_t, atomic_reference_>;

#pragma endregion Private Types

#pragma region Private Methods

    /** Words @p bytes take, rounded up. */
    [[nodiscard]] static constexpr std::size_t words_of_(std::size_t bytes) noexcept {
        return (bytes + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);
    }

    [[nodiscard]] std::size_t slot_of_(stored_key_t const& stored_key, transaction_stamp_t stamp) const noexcept {
        return (stored_key_hash_t {}(stored_key) ^ st::hash<transaction_stamp_t> {}(stamp)) & (slots_.size() - 1);
    }

    /** The words @p slot owns, allocated with a zero header by the first fill and kept until the
     *  cache dies, or null when the budget refuses them; two first fills racing keep the words of
     *  whichever publishes first. */
    [[nodiscard]] std::uint64_t* words_of_slot_(std::uint64_t*& slot) noexcept {
        atomic_reference_<std::uint64_t*> const published {slot};
        if (std::uint64_t* const words = published.load(std::memory_order_acquire)) return words;
        words_allocator_t allocator {slots_.get_allocator()};
        std::uint64_t* const allocated = allocator.allocate(slot_words_);
        if (!allocated) return nullptr;
        std::memset(allocated, 0, value_word_k * sizeof(std::uint64_t));
        std::uint64_t* first = nullptr;
        if (published.compare_exchange_strong(first, allocated, std::memory_order_acq_rel, std::memory_order_acquire))
            return allocated;
        allocator.deallocate(allocated, slot_words_);
        return first;
    }

    [[nodiscard]] static std::uint64_t load_(std::uint64_t const& word,
                                             std::memory_order order = std::memory_order_relaxed) noexcept {
        return atomic_reference_<std::uint64_t const> {word}.load(order);
    }

    static void store_(std::uint64_t& word, std::uint64_t value) noexcept {
        atomic_reference_<std::uint64_t> {word}.store(value, std::memory_order_relaxed);
    }

    /** Stores @p bytes into the words from @p words on, one atomic word at a time, the last
     *  padded with zeros. */
    static void store_bytes_(std::uint64_t* words, std::span<byte_t const> bytes) noexcept {
        for (std::size_t offset = 0; offset < bytes.size(); offset += sizeof(std::uint64_t)) {
            std::uint64_t word = 0;
            std::memcpy(&word, bytes.data() + offset, fu::min_of(sizeof(word), bytes.size() - offset));
            store_(words[offset / sizeof(word)], word);
        }
    }

    /** Fills @p bytes from the words from @p words on, one atomic word at a time. */
    static void load_bytes_(std::uint64_t const* words, std::span<byte_t> bytes) noexcept {
        for (std::size_t offset = 0; offset < bytes.size(); offset += sizeof(std::uint64_t)) {
            std::uint64_t const word = load_(words[offset / sizeof(word)]);
            std::memcpy(bytes.data() + offset, &word, fu::min_of(sizeof(word), bytes.size() - offset));
        }
    }

#pragma endregion Private Methods

#pragma region Private Members

    /** The word of a slot holding its sequence: zero before its first fill, odd while a
     *  fill writes it. */
    static constexpr std::size_t sequence_word_k = 0;

    /** The word of a slot holding the stamp of the version it caches. */
    static constexpr std::size_t stamp_word_k = 1;

    /** The word of a slot holding how many bytes the value takes. */
    static constexpr std::size_t size_word_k = 2;

    /** The first word of a slot holding the key. */
    static constexpr std::size_t key_word_k = 3;

    /** The first word of a slot holding the value, after the key. */
    static constexpr std::size_t value_word_k = key_word_k + (sizeof(stored_key_t) + sizeof(std::uint64_t) - 1) /
                                                                 sizeof(std::uint64_t);

    /** The slots, each naming the words it owns or null. */
    slots_t slots_;

    /** Words every slot owns: the header, the key and the widest value. */
    std::size_t slot_words_ = 0;

#pragma endregion Private Members
};

#pragma endregion Cache

#pragma endregion Value Cache

} // namespace unum::ustore::blobs
