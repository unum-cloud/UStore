/**
 *  @file include/ustore/utilities/nvme_ring.hpp
 *  @author Ash Vardanian
 *  @date May 23, 2021
 *  @brief The submission and completion ring every NVMe transport exposes, and the vocabulary beneath it: aligned
 *      and budgeted allocators, completion counters, 128-bit identifiers, extents and serialization.
 *
 *  Nothing here reaches StringZilla, so a CUDA translation unit can include the ring and the key slots above it;
 *  text and its serialization start at @c nvme_volume.hpp.
 *
 *  The ring is exposed rather than hidden behind a blocking read, because a blocking read reaches
 *  the depth of one thread and an NVMe namespace wants thousands of commands outstanding. An queue pair
 *  submits a batch, does other work, and polls its own completions; nothing here ever blocks, so no
 *  fiber is needed to park a caller that ran out of credit.
 *
 *  The shape is what the three real transports allow rather than what is convenient. io_uring with
 *  @c SINGLE_ISSUER and @c DEFER_TASKRUN answers @c -EEXIST when a thread other than the submitter
 *  polls, and an SPDK queue pair silently corrupts its tracker list in the same case, so a queue pair is
 *  a single-threaded queue pair rather than a label. Both address a buffer by a registered memory slot plus an
 *  offset - @c IORING_OP_READ_FIXED and @c cuFileRead alike - so a request names a registered memory and
 *  not a pointer. Neither reports a misaligned request at submission time, so every per-request
 *  defect arrives as a failed completion and @c submit refuses only what would corrupt bookkeeping.
 */

#pragma once

#include <cassert> // `assert`
#include <cstddef> // `std::byte`, `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint16_t`, `std::uint64_t`, `std::uintptr_t`
#include <cstdlib> // `std::aligned_alloc`, `std::free`
#include <cstring> // `std::memcpy`

#if defined(_WIN32)
#include <malloc.h>  // `_aligned_malloc`, `_aligned_free`
#include <windows.h> // `NTSTATUS`, which <bcrypt.h> needs
#include <bcrypt.h>  // `BCryptGenRandom`
#else
#include <sys/random.h> // `getentropy`
#endif

#include <atomic>      // `std::memory_order`
#include <chrono>      // `std::chrono::system_clock`
#include <compare>     // `std::strong_ordering`
#include <concepts>    // `std::same_as`
#include <functional>  // `std::hash`
#include <limits>      // `std::numeric_limits`
#include <memory>      // `std::allocator_traits`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::true_type`, `std::is_trivially_copyable_v`
#include <utility>     // `std::exchange`, `std::move`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`
#include <smashtable/shared.hpp>       // `st::status_t`, `st::default_allocator`

#include "ustore/utilities/types.hpp" // `status_t`, `status_code_t`, `expected`, `byte_t`

namespace unum::ustore::utilities {

#pragma region Aligned Allocator

/**
 *  @brief Places every allocation on an @p alignment_ boundary, and rounds its size up to one.
 *
 *  Unbuffered reads and writes accept a buffer only when its address and its length are both
 *  multiples of the device's block size, which is what this allocator guarantees for every count.
 *  Exhaustion answers @c nullptr, as every allocator here does.
 */
template <typename value_type_, std::size_t alignment_>
class aligned_allocator {
  public:
    using value_type = value_type_;
    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal = std::true_type;

    /** The boundary every allocation starts on, and the multiple its size is rounded up to. */
    static constexpr std::size_t alignment_k = alignment_;

    static_assert(alignment_k != 0 && (alignment_k & (alignment_k - 1)) == 0,
                  "An alignment is a power of two, as every block size is");

    constexpr aligned_allocator() noexcept = default;

    /** The rebinding conversion @c std::allocator_traits performs; the alignment is fixed. */
    template <typename other_type_>
    constexpr aligned_allocator(aligned_allocator<other_type_, alignment_> const&) noexcept {}

    /** Allocates room for @p count values on an aligned boundary, or answers @c nullptr. */
    [[nodiscard]] value_type_* allocate(std::size_t count) noexcept {
        if (count == 0 || count > max_count_k) return nullptr;
#if defined(_WIN32)
        return static_cast<value_type_*>(::_aligned_malloc(aligned_bytes(count), alignment_k));
#else
        return static_cast<value_type_*>(std::aligned_alloc(alignment_k, aligned_bytes(count)));
#endif
    }

    /** Releases @p pointer, which must come from @c allocate. */
    void deallocate(value_type_* pointer, std::size_t) noexcept {
#if defined(_WIN32)
        ::_aligned_free(pointer);
#else
        std::free(pointer);
#endif
    }

    /** The bytes @p count values occupy once rounded up to the alignment. */
    static constexpr std::size_t aligned_bytes(std::size_t count) noexcept {
        return (count * sizeof(value_type_) + alignment_k - 1) / alignment_k * alignment_k;
    }

    template <typename other_type_>
    constexpr bool operator==(aligned_allocator<other_type_, alignment_> const&) const noexcept {
        return true;
    }

  private:

    /** The largest count whose aligned byte size still fits a @c std::size_t. */
    static constexpr std::size_t max_count_k = (std::numeric_limits<std::size_t>::max() - alignment_k) /
                                               sizeof(value_type_);
};

#pragma endregion Aligned Allocator

#pragma region Constrained Allocator

/** A budget with no limit, which every allocation fits inside. */
inline constexpr std::size_t unlimited_resource_k = static_cast<std::size_t>(-1);

/**
 *  @brief The bytes one named part of the engine may hold, and the bytes it holds now.
 *
 *  A budget is acquired and released in bytes by however many threads share it, each acquisition
 *  bounded so the budget can never be overdrawn and an exhausted one refuses instead of waiting.
 *  Whoever owns the budget outlives every allocator charging it.
 *
 *  @tparam atomic_reference_ The reference the available bytes are owned through, for one operation at a time.
 */
template <template <typename> typename atomic_reference_>
class resource_constraint {
  public:

    /** A budget of @p capacity bytes, all of them available. */
    explicit resource_constraint(std::size_t capacity = unlimited_resource_k) noexcept
        : capacity_(capacity), available_(capacity) {}

    resource_constraint(resource_constraint const&) = delete;
    resource_constraint& operator=(resource_constraint const&) = delete;

    /** The bytes the budget allows in total. */
    std::size_t capacity() const noexcept { return capacity_; }

    /** The bytes the budget still allows. */
    std::size_t available() const noexcept {
        return atomic_reference_<std::size_t const> {available_}.load(std::memory_order_acquire);
    }

    /** The bytes the budget currently holds. */
    std::size_t used() const noexcept { return capacity_ - available(); }

    /** Whether @p bytes would still fit, which a later acquisition may no longer find true. */
    bool has_more(std::size_t bytes) const noexcept { return bytes <= available(); }

    /** Charges @p bytes to the budget, answering whether they fit. */
    bool acquire(std::size_t bytes) noexcept {
        atomic_reference_<std::size_t> const reference {available_};
        std::size_t const observed = reference.fetch_sub_if_at_least(bytes, 0, std::memory_order_acquire);
        return observed >= bytes;
    }

    /** Returns @p bytes to the budget, answering whether they had been charged to it. */
    bool release(std::size_t bytes) noexcept {
        atomic_reference_<std::size_t> const reference {available_};
        std::size_t const observed = reference.fetch_add_if_at_most(bytes, capacity_, std::memory_order_release);
        return observed + bytes <= capacity_;
    }

  private:

    /** The bytes the budget allows in total, fixed when it is named. */
    std::size_t capacity_;

    /** The bytes the budget still allows, which every acquisition and release moves. */
    std::size_t available_;
};

/**
 *  @brief An allocator that charges a named budget, and refuses once the budget is spent.
 *
 *  Exhaustion answers @c nullptr, which is what every container here already checks, so a budget
 *  reaches a caller as a status rather than as an exception. The budget is borrowed, never owned.
 *
 *  @tparam atomic_reference_ The reference the budget owns its word through.
 */
template <typename value_type_, template <typename> typename atomic_reference_,
          typename base_allocator_type_ = st::default_allocator<value_type_>>
class constrained_allocator {
  public:
    using value_type = value_type_;
    using base_allocator_t = base_allocator_type_;

    /** The budget every allocation is charged to. */
    using constraint_t = resource_constraint<atomic_reference_>;

    /** A moved container keeps charging the budget its allocator named. */
    using propagate_on_container_move_assignment = std::true_type;

    /** The same budget over values of @p other_type_, drawn from the base allocator rebound alike. */
    template <typename other_type_>
    struct rebind {
        using other =
            constrained_allocator<other_type_, atomic_reference_,
                                  typename std::allocator_traits<base_allocator_t>::template rebind_alloc<other_type_>>;
    };

    /** An allocator naming no budget, which refuses every allocation until a named one replaces it. */
    constrained_allocator() noexcept : base_(), constraint_(nullptr) {}

    /** An allocator charging @p constraint, which must outlive it. */
    explicit constrained_allocator(constraint_t& constraint, base_allocator_t base = {}) noexcept
        : base_(base), constraint_(&constraint) {}

    /** The rebinding conversion @c std::allocator_traits performs; the budget travels with it. */
    template <typename other_type_, typename other_base_type_>
    constrained_allocator(constrained_allocator<other_type_, atomic_reference_, other_base_type_> const& other) noexcept
        : base_(), constraint_(other.constraint_pointer()) {}

    /** Charges room for @p count values, or answers @c nullptr when the budget refuses them. */
    [[nodiscard]] value_type_* allocate(std::size_t count) noexcept {
        if (!constraint_ || !constraint_->acquire(count * sizeof(value_type_))) return nullptr;
        value_type_* const memory = base_.allocate(count);
        if (!memory) { [[maybe_unused]] bool const returned = constraint_->release(count * sizeof(value_type_)); }
        return memory;
    }

    /** Releases @p pointer and returns its bytes to the budget. */
    void deallocate(value_type_* pointer, std::size_t count) noexcept {
        base_.deallocate(pointer, count);
        if (constraint_) { [[maybe_unused]] bool const returned = constraint_->release(count * sizeof(value_type_)); }
    }

    /** The budget this allocator charges. */
    constraint_t& constraint() const noexcept { return *constraint_; }

    /** The budget this allocator charges, or null while it names none. */
    constraint_t* constraint_pointer() const noexcept { return constraint_; }

    template <typename other_type_, typename other_base_type_>
    bool operator==(
        constrained_allocator<other_type_, atomic_reference_, other_base_type_> const& other) const noexcept {
        return constraint_ == other.constraint_pointer();
    }

  private:

    /** The allocator the charged bytes actually come from. */
    base_allocator_t base_;

    /** The budget every allocation is charged to, owned by whoever named it. */
    constraint_t* constraint_;
};

#pragma endregion Constrained Allocator

#pragma region Countdown

template <template <typename> typename atomic_reference_, typename waiter_type_>
class notifier;

/**
 *  @brief The parts still owed by one fanned-out job, and the first failure any of them reported.
 *
 *  One owner holds the counter and waits on it; every part holds a @ref notifier onto it and
 *  reports through that, so the counter itself is never copied and outlives every part.
 *
 *  @tparam atomic_reference_ The reference the counter's words are owned through.
 *  @tparam waiter_type_ What the waiting owner's core does between polls.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class countdown {
  public:

    countdown() noexcept = default;

    /** A counter owed @p parts reports. */
    explicit countdown(std::size_t parts) noexcept : remaining_(static_cast<std::int32_t>(parts)) {}

    countdown(countdown&&) = default;
    countdown& operator=(countdown&&) = default;
    countdown(countdown const&) = delete;
    countdown& operator=(countdown const&) = delete;

    /** Arms the counter for @p parts reports, forgetting whatever the last round recorded. */
    void reset(std::size_t parts = 1) noexcept {
        message_ = nullptr;
        code_reference_().store(0, std::memory_order_relaxed);
        remaining_reference_().store(static_cast<std::int32_t>(parts), std::memory_order_release);
    }

    /** Adds @p parts to what the counter is owed, for a fan-out that grows while it runs. */
    void increment(std::int32_t parts = 1) noexcept { remaining_reference_().add(parts, std::memory_order_relaxed); }

    /** Whether every part has reported, or one of them has failed. */
    bool has_finished() const noexcept {
        if (code_reference_().load(std::memory_order_acquire) != 0) return true;
        return remaining_reference_().load(std::memory_order_acquire) <= 0;
    }

    /** Whether any part reported a failure. */
    bool has_failed() const noexcept { return code_reference_().load(std::memory_order_acquire) != 0; }

    /** Whether the parts still owed have fallen to @p threshold or below. */
    bool test(std::int32_t threshold) const noexcept {
        return remaining_reference_().load(std::memory_order_acquire) <= threshold;
    }

    /**
     *  @brief Waits until every part reports or one fails.
     *
     *  @return Whether every part succeeded.
     */
    bool wait() noexcept {
        for (std::int32_t remaining = remaining_reference_().load(std::memory_order_acquire); remaining > 0;
             remaining = remaining_reference_().load(std::memory_order_acquire))
            waiter_type_ {}(&remaining_, remaining, std::size_t {0});
        return !has_failed();
    }

    /** The failure a part reported, and success once the counter carries none again. */
    status_t release_status() noexcept {
        int const code = code_reference_().exchange(0, std::memory_order_acquire);
        if (code == 0) return {};
        return {static_cast<status_code_t>(code), std::exchange(message_, nullptr)};
    }

  private:

    friend class notifier<atomic_reference_, waiter_type_>;

    /** The parts still owed, which a failure drives to zero however many are outstanding. */
    std::int32_t remaining_ = 1;

    /** The @c ustore_status_t of the first failure, or zero while none has been reported. */
    int code_ = 0;

    /** The static message of the first failure, stored before the code that publishes it. */
    char const* message_ = nullptr;

    atomic_reference_<std::int32_t> remaining_reference_() noexcept {
        return atomic_reference_<std::int32_t> {remaining_};
    }

    atomic_reference_<std::int32_t const> remaining_reference_() const noexcept {
        return atomic_reference_<std::int32_t const> {remaining_};
    }

    atomic_reference_<int> code_reference_() noexcept { return atomic_reference_<int> {code_}; }

    atomic_reference_<int const> code_reference_() const noexcept { return atomic_reference_<int const> {code_}; }
};

/**
 *  @brief One part's handle onto the counter its job reports through, cheap enough to copy per part.
 *
 *  Every handle borrows a counter that outlives it, so a part reports without knowing who waits.
 */
template <template <typename> typename atomic_reference_, typename waiter_type_>
class notifier {
  public:

    /** The counter a handle reports through. */
    using countdown_t = countdown<atomic_reference_, waiter_type_>;

    notifier() = delete;
    notifier(notifier const&) = default;

    /** A handle onto @p counter, which must outlive every part holding one. */
    notifier(countdown_t& counter) noexcept : countdown_(counter) {}

    /** The counter this handle reports through. */
    countdown_t& counter() const noexcept { return countdown_; }

    /** Reports one part done. */
    void did_one() const noexcept { countdown_.remaining_reference_().sub(1, std::memory_order_release); }

    /** Reports every remaining part done, for a fan-out that finished early. */
    void did_all() const noexcept { countdown_.remaining_reference_().store(0, std::memory_order_release); }

    /**
     *  @brief Reports @p status, which the counter keeps if it is the first failure.
     *
     *  A failure also drives the remaining parts to zero, so a waiter stops without waiting for
     *  the parts a failed job will never run. Reporting success does nothing.
     */
    void fail(status_t status) const noexcept {
        if (status.succeeded()) return;
        countdown_.message_ = status.message;
        int expected = 0;
        if (!countdown_.code_reference_().compare_exchange_strong(expected, static_cast<int>(status.code),
                                                                  std::memory_order_release, std::memory_order_relaxed))
            return;
        countdown_.remaining_reference_().store(0, std::memory_order_release);
    }

    /** Whether any part reported a failure. */
    bool has_failed() const noexcept { return countdown_.has_failed(); }

    /** Whether every part has reported, or one of them has failed. */
    bool has_finished() const noexcept { return countdown_.has_finished(); }

    /** Whether the parts still owed have fallen to @p threshold or below. */
    bool test(std::int32_t threshold) const noexcept { return countdown_.test(threshold); }

  private:

    /** The counter this handle reports through, owned by whoever waits on it. */
    countdown_t& countdown_;
};

#pragma endregion Countdown

#pragma region UUID

/** The bit pattern of octet 8, which says whose layout the remaining bytes follow. */
enum class uuid_variant_t : std::uint8_t {

    /** The @c 0xxx pattern of the Apollo Network Computing System. */
    network_computing_system_k,

    /** The @c 10xx pattern of RFC 4122, which every identifier this engine mints carries. */
    rfc_k,

    /** The @c 110x pattern of the Component Object Model. */
    microsoft_k,

    /** The @c 111x pattern, which no specification claims. */
    reserved_k,
};

/**
 *  @brief A 128-bit identifier held as sixteen bytes, printed as the canonical 36 characters.
 *
 *  A zero identifier names nothing, which is what a default-constructed one holds and what
 *  @c is_valid reports; every minted identifier is version 4, so all but six of its bits are random.
 */
struct uuid_t {

    /** Bytes the identifier occupies in memory and on disk. */
    static constexpr std::size_t size_bytes_k = 16;

    /** Characters the canonical @c 8-4-4-4-12 text form occupies, dashes included. */
    static constexpr std::size_t printed_length_k = 36;

    /** The sixteen bytes, in the order they are printed and stored. */
    std::uint8_t bytes[size_bytes_k] = {};

    uuid_t() = default;

    /** Reads an identifier from the sixteen bytes at @p source. */
    explicit uuid_t(std::span<std::uint8_t const, size_bytes_k> source) noexcept {
        std::memcpy(bytes, source.data(), size_bytes_k);
    }

    /** Whose layout the remaining bytes follow, read off octet 8. */
    constexpr uuid_variant_t variant() const noexcept {
        if ((bytes[8] & 0x80) == 0x00) return uuid_variant_t::network_computing_system_k;
        if ((bytes[8] & 0xC0) == 0x80) return uuid_variant_t::rfc_k;
        if ((bytes[8] & 0xE0) == 0xC0) return uuid_variant_t::microsoft_k;
        return uuid_variant_t::reserved_k;
    }

    /** Whether the identifier names anything, which a zero one does not. */
    constexpr bool is_valid() const noexcept {
        std::uint8_t any = 0;
        for (std::size_t index = 0; index != size_bytes_k; ++index) any |= bytes[index];
        return any != 0;
    }

    /** The first eight bytes read as a big-endian word, which orders identifiers as @c memcmp does. */
    constexpr std::uint64_t high_word() const noexcept { return word_(0); }

    /** The last eight bytes read as a big-endian word. */
    constexpr std::uint64_t low_word() const noexcept { return word_(8); }

    friend constexpr bool operator==(uuid_t const&, uuid_t const&) = default;

    friend constexpr std::strong_ordering operator<=>(uuid_t const& first, uuid_t const& second) noexcept {
        if (std::strong_ordering const high = first.high_word() <=> second.high_word(); high != 0) return high;
        return first.low_word() <=> second.low_word();
    }

    /** Writes the canonical @c 8-4-4-4-12 text form into @p destination, without a terminator. */
    constexpr void print(std::span<char, printed_length_k> destination) const noexcept {
        std::size_t written = 0;
        for (std::size_t index = 0; index != size_bytes_k; ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10) destination[written++] = '-';
            destination[written++] = digits_k[bytes[index] >> 4];
            destination[written++] = digits_k[bytes[index] & 0x0F];
        }
    }

    /** Whether @p text is 36 characters of the canonical form, so @c parse would read it whole. */
    static constexpr bool parses(std::span<char const, printed_length_k> text) noexcept {
        std::size_t read = 0;
        for (std::size_t index = 0; index != size_bytes_k; ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10)
                if (text[read++] != '-') return false;
            if (value_of_(text[read++]) == invalid_digit_k) return false;
            if (value_of_(text[read++]) == invalid_digit_k) return false;
        }
        return true;
    }

    /** Reads the canonical form out of @p text, which @c parses has already accepted. */
    static constexpr uuid_t parse(std::span<char const, printed_length_k> text) noexcept {
        uuid_t identifier;
        std::size_t read = 0;
        for (std::size_t index = 0; index != size_bytes_k; ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10) ++read;
            std::uint8_t const high = value_of_(text[read++]);
            std::uint8_t const low = value_of_(text[read++]);
            identifier.bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
        }
        return identifier;
    }

  private:

    /** The lowercase hexadecimal alphabet the text form prints. */
    static constexpr char digits_k[17] = "0123456789abcdef";

    /** What @c value_of_ answers for a character that is no hexadecimal digit. */
    static constexpr std::uint8_t invalid_digit_k = 0xFF;

    /** The value of one hexadecimal digit, in either case. */
    static constexpr std::uint8_t value_of_(char digit) noexcept {
        if (digit >= '0' && digit <= '9') return static_cast<std::uint8_t>(digit - '0');
        if (digit >= 'a' && digit <= 'f') return static_cast<std::uint8_t>(digit - 'a' + 10);
        if (digit >= 'A' && digit <= 'F') return static_cast<std::uint8_t>(digit - 'A' + 10);
        return invalid_digit_k;
    }

    /** The eight bytes at @p offset read as one big-endian word. */
    constexpr std::uint64_t word_(std::size_t offset) const noexcept {
        std::uint64_t word = 0;
        for (std::size_t index = 0; index != 8; ++index) word = (word << 8) | bytes[offset + index];
        return word;
    }
};

static_assert(sizeof(uuid_t) == uuid_t::size_bytes_k, "An identifier is its sixteen bytes and nothing else");

/** Fills @p destination with bytes from the operating system, answering whether it could. */
inline bool fill_with_entropy(std::span<std::uint8_t> destination) noexcept {
#if defined(_WIN32)
    return ::BCryptGenRandom(nullptr, destination.data(), static_cast<ULONG>(destination.size()),
                             BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
#else
    return ::getentropy(destination.data(), destination.size()) == 0;
#endif
}

/**
 *  @brief Mints a version 4 identifier from the entropy the operating system hands out.
 *
 *  Answers a zero identifier, which @c is_valid rejects, when the system refuses entropy; the
 *  caller learns of that through the identifier rather than through a status, since every source
 *  of a fresh identifier already validates it before recording it.
 */
inline uuid_t uuid_generator() noexcept {
    uuid_t identifier;
    if (!fill_with_entropy(identifier.bytes)) return {};
    identifier.bytes[6] = static_cast<std::uint8_t>((identifier.bytes[6] & 0x0F) | 0x40);
    identifier.bytes[8] = static_cast<std::uint8_t>((identifier.bytes[8] & 0x3F) | 0x80);
    return identifier;
}

/** The identifier folded onto one word, for a table keyed by a number rather than by the bytes. */
constexpr std::uint64_t uuid_to_u64(uuid_t const& identifier) noexcept {
    std::uint64_t mixed = identifier.high_word() ^ (identifier.low_word() * 0x9E3779B97F4A7C15ull);
    mixed ^= mixed >> 30;
    mixed *= 0xBF58476D1CE4E5B9ull;
    mixed ^= mixed >> 27;
    mixed *= 0x94D049BB133111EBull;
    return mixed ^ (mixed >> 31);
}

#pragma endregion UUID

#pragma region Shared Vocabulary

/** Alignment and size multiple of buffers used for direct I/O. */
inline constexpr std::size_t buffer_alignment_k = 4096;

/** Attempts of a disk read before it fails. */
inline constexpr std::size_t disk_read_attempts_on_failure_k = 2;

/** Attempts of a disk write before it fails. */
inline constexpr std::size_t disk_write_attempts_on_failure_k = 4;

#pragma region Vocabulary

/** Bytes a caller may write into. */
using bytes_span_t = std::span<std::byte>;

/** Bytes a caller may only read. */
using bytes_view_t = std::span<std::byte const>;

/** The allocator every unbudgeted container reaches, which answers null instead of throwing. */
template <typename value_type_>
using heap_allocator = st::default_allocator<value_type_>;

/** The allocator of buffers unbuffered reads and writes accept. */
template <typename value_type_>
using direct_io_allocator = aligned_allocator<value_type_, buffer_alignment_k>;

/** Bytes charged to no budget, with no alignment beyond the heap's own. */
using buffer_t = st::basic_vector<std::byte>;

/** Bytes of a buffer unbuffered I/O accepts, charged to no budget. */
using aligned_buffer_t = st::basic_vector<std::byte, direct_io_allocator<std::byte>>;

/** Bytes charged to a named budget. */
template <template <typename> typename atomic_reference_>
using constrained_buffer = st::basic_vector<std::byte, constrained_allocator<std::byte, atomic_reference_>>;

/** Bytes of a buffer unbuffered I/O accepts, charged to a named budget. */
template <template <typename> typename atomic_reference_>
using constrained_aligned_buffer =
    st::basic_vector<std::byte, constrained_allocator<std::byte, atomic_reference_, direct_io_allocator<std::byte>>>;

#pragma endregion Vocabulary

#pragma region Time

/** The clock every timestamp of the engine is taken from. */
using clock_t = std::chrono::system_clock;

/** One instant on that clock, as a snapshot, a transaction and a watch record it. */
using time_point_t = clock_t::time_point;

/** The instant this call happens at. */
inline time_point_t timestamp() noexcept { return clock_t::now(); }

/** Hashes an instant by the count of its epoch offset, for a table keyed by one. */
struct time_point_hasher_t {

    std::size_t operator()(time_point_t const& instant) const noexcept {
        return std::hash<i64_t> {}(static_cast<i64_t>(instant.time_since_epoch().count()));
    }
};

#pragma endregion Time

#pragma region Failures

/** The status reporting a SmashTable container's @p failure, which every fallible member answers. */
constexpr status_t to_status(st::status_t failure) noexcept {
    switch (failure) {
    case st::status_t::success_k: return {};
    case st::status_t::out_of_memory_heap_k: return {status_code_t::out_of_memory_k, "Out of memory"};
    case st::status_t::consistency_k: return {status_code_t::internal_k, "A container invariant did not hold"};
    case st::status_t::operation_not_permitted_k:
        return {status_code_t::invalid_argument_k, "The object does not admit the call"};
    case st::status_t::operation_would_block_k: return {status_code_t::internal_k, "A bounded retry gave up"};
    case st::status_t::invalid_argument_k: return {status_code_t::invalid_argument_k, "Invalid argument"};
    case st::status_t::capacity_exhausted_k: return {status_code_t::capacity_exhausted_k, "Capacity exhausted"};
    case st::status_t::input_output_k: return {status_code_t::io_k, "Input or output failed"};
    case st::status_t::key_already_exists_k: return {status_code_t::already_exists_k, "Key already exists"};
    case st::status_t::key_not_found_k: return {status_code_t::not_found_k, "Key not found"};
    case st::status_t::write_conflict_k: return {status_code_t::write_conflict_k, "Write conflict"};
    case st::status_t::read_conflict_k: return {status_code_t::read_conflict_k, "Read conflict"};
    case st::status_t::phantom_conflict_k: return {status_code_t::phantom_conflict_k, "Phantom conflict"};
    default: return {status_code_t::internal_k, "A container failed for an unknown reason"};
    }
}

#pragma endregion Failures

#pragma region Measurements

/** Memory and disk space one component takes. */
struct storage_info_t {

    /** Bytes mapped into memory. */
    std::size_t mapped_bytes = 0;

    /** Bytes taken on disk. */
    std::size_t disk_bytes = 0;
};

#pragma endregion Measurements

#pragma region Atomic Words

/** A word one thread claims by compare-exchange: readers count upward from zero, the writer holds @c writer_claim_k. */
using claim_word_t = std::int32_t;

/** What a claim word holds while one writer has it, so no reader can add to it. */
constexpr claim_word_t writer_claim_k = -1;

/** Readers still inside a retired slot, which a publisher waits on before freeing it. */
using readers_count_t = std::int32_t;

/** Whether the owner of a background job or a merge still wants it, moved by compare-exchange on a word. */
enum class cancellation_state_t : std::int32_t {
    wanted_k = 0,
    cancelled_k = 1,
};

/** Whether a thread holds the right to compact a tree, moved by compare-exchange on a word. */
enum class compaction_state_t : std::int32_t {
    idle_k = 0,
    compacting_k = 1,
};

#pragma endregion Atomic Words

#pragma region Byte Views

/** Whether both the address and the size of @p buffer are multiples of @c buffer_alignment_k. */
inline bool is_buffer_aligned(bytes_view_t buffer) noexcept {
    auto const address = reinterpret_cast<std::uintptr_t>(buffer.data());
    return address % buffer_alignment_k == 0 && buffer.size() % buffer_alignment_k == 0;
}

/** The bytes of @p buffer, which a caller may only read. */
template <typename allocator_type_>
inline bytes_view_t as_bytes_view(st::basic_vector<std::byte, allocator_type_> const& buffer) noexcept {
    return bytes_view_t {buffer};
}

/** The bytes of @p buffer, which a caller may write into. */
template <typename allocator_type_>
inline bytes_span_t as_bytes_span(st::basic_vector<std::byte, allocator_type_>& buffer) noexcept {
    return bytes_span_t {buffer};
}

#pragma endregion Byte Views

#pragma region Serialization

/** A value that measures, saves and loads its own bytes, where a successful load consumes its storage requirements. */
template <typename value_type_>
concept serializable_concept = requires(value_type_ const& constant, value_type_& value, bytes_span_t destination,
                                        bytes_view_t source) {
    { constant.storage_requirements() } noexcept -> std::same_as<std::size_t>;
    { constant.save(destination) } noexcept -> std::same_as<status_t>;
    { value.load(source) } noexcept -> std::same_as<status_t>;
};

/** A value stored as its object representation, having no serialization of its own. */
template <typename value_type_>
concept trivially_serializable_concept = std::is_trivially_copyable_v<value_type_> &&
                                         !serializable_concept<value_type_>;

/** The bytes of a trivially serializable value. */
template <trivially_serializable_concept value_type_>
std::size_t storage_requirements(value_type_ const&) noexcept {
    return sizeof(value_type_);
}

/** Copies the object representation of @p value into @p destination. */
template <trivially_serializable_concept value_type_>
status_t save(value_type_ const& value, bytes_span_t destination) noexcept {
    if (destination.size() < sizeof(value_type_))
        return status_t {status_code_t::invalid_argument_k, "Buffer is too small"};
    std::memcpy(destination.data(), &value, sizeof(value_type_));
    return {};
}

/** Copies the object representation of @p value out of @p source. */
template <trivially_serializable_concept value_type_>
status_t load(value_type_& value, bytes_view_t source) noexcept {
    if (source.size() < sizeof(value_type_)) return status_t {status_code_t::malformed_k, "Corrupted data"};
    std::memcpy(&value, source.data(), sizeof(value_type_));
    return {};
}

/** The bytes a serializable value reports. */
template <serializable_concept value_type_>
std::size_t storage_requirements(value_type_ const& value) noexcept {
    return value.storage_requirements();
}

/** Saves a serializable value through its own member. */
template <serializable_concept value_type_>
status_t save(value_type_ const& value, bytes_span_t destination) noexcept {
    return value.save(destination);
}

/** Loads a serializable value through its own member. */
template <serializable_concept value_type_>
status_t load(value_type_& value, bytes_view_t source) noexcept {
    return value.load(source);
}

/** The bytes of an array: its 64-bit count, then every element. */
template <typename element_type_, typename allocator_type_>
std::size_t storage_requirements(st::basic_vector<element_type_, allocator_type_> const& elements) noexcept {
    std::size_t bytes = sizeof(std::uint64_t);
    for (element_type_ const& element : elements) bytes += storage_requirements(element);
    return bytes;
}

/** Saves the 64-bit count of @p elements, then every element back to back. */
template <typename element_type_, typename allocator_type_>
status_t save(st::basic_vector<element_type_, allocator_type_> const& elements, bytes_span_t destination) noexcept {
    std::uint64_t const count = elements.size();
    if (status_t const status = save(count, destination); status.failed()) return status;
    std::size_t offset = sizeof(count);
    for (element_type_ const& element : elements) {
        if (status_t const status = save(element, destination.subspan(offset)); status.failed()) return status;
        offset += storage_requirements(element);
    }
    return {};
}

/** Loads an array saved as its 64-bit count, then every element back to back. */
template <typename element_type_, typename allocator_type_>
status_t load(st::basic_vector<element_type_, allocator_type_>& elements, bytes_view_t source) noexcept {
    std::uint64_t count = 0;
    if (status_t const status = load(count, source); status.failed()) return status;
    std::size_t offset = sizeof(count);
    // Every element takes at least one byte, so a larger count is corrupt.
    if (count > source.size() - offset) return status_t {status_code_t::malformed_k, "Corrupted data"};
    if (st::status_t const status = elements.resize(count); status != st::status_t::success_k) return to_status(status);
    for (element_type_& element : elements) {
        if (status_t const status = load(element, source.subspan(offset)); status.failed()) return status;
        offset += storage_requirements(element);
    }
    return {};
}

/** The bytes of @p fields stored back to back. */
template <typename... field_types_>
std::size_t fields_storage_requirements(field_types_ const&... fields) noexcept {
    return (std::size_t {0} + ... + storage_requirements(fields));
}

/** Saves no fields. */
inline status_t save_fields(bytes_span_t) noexcept { return {}; }

/** Saves @p field, then @p fields, back to back into @p destination. */
template <typename field_type_, typename... field_types_>
status_t save_fields(bytes_span_t destination, field_type_ const& field, field_types_ const&... fields) noexcept {
    if (status_t const status = save(field, destination); status.failed()) return status;
    return save_fields(destination.subspan(storage_requirements(field)), fields...);
}

/** Loads no fields. */
inline status_t load_fields(bytes_view_t) noexcept { return {}; }

/** Loads @p field, then @p fields, stored back to back in @p source. */
template <typename field_type_, typename... field_types_>
status_t load_fields(bytes_view_t source, field_type_& field, field_types_&... fields) noexcept {
    if (status_t const status = load(field, source); status.failed()) return status;
    return load_fields(source.subspan(storage_requirements(field)), fields...);
}

#pragma endregion Serialization

#pragma endregion Shared Vocabulary

#pragma region IO Types

/** One mounted disk of a data directory, where every stored value names the disk it sits on. */
enum class disk_id_t : std::uint16_t {
    unknown_k = 0xFFFF,
};

/** A POSIX file descriptor, where a negative value marks a failed open. */
enum class file_id_t : int {
    invalid_k = -1,
};

/** Whether a file operation blocks the caller or completes through @c io_uring. */
enum class execution_policy_t {
    sync_k,
    async_k,
};

#pragma endregion IO Types

#pragma region Extent

#pragma region Alignment

/**
 *  @brief The alignment every transport accepts, for memory, for offsets and for lengths alike.
 *
 *  A caller that cannot ask its disk what it needs aligns to this and is accepted everywhere:
 *  io_uring on @c O_DIRECT reports @b [512,4096] through @c STATX_DIOALIGN, SPDK needs a page
 *  boundary for any transfer that crosses one, and cuFile wants 4096 on the device pointer, the
 *  file offset and the size. Nothing hardcodes 512, since a disk reports its own three numbers.
 */
inline constexpr std::uint64_t portable_alignment_k = 4096;

/** Whether @p address is a multiple of @p alignment, for the memory a registration hands the driver. */
inline bool is_address_aligned(void const* address, std::uint64_t alignment) noexcept {
    return (reinterpret_cast<std::uintptr_t>(address) & (alignment - 1)) == 0;
}

#pragma endregion Alignment

#pragma region Extent

/**
 *  @brief A contiguous byte range on one volume, counted from the volume's first byte.
 *
 *  One disk serves one volume, so an extent needs no volume identifier: a directory volume
 *  resolves a name to its disk before it builds the extent, and an extent volume addresses the
 *  namespace directly. Both fields are byte counts, never blocks, so a 512-byte and a 4096-byte
 *  format describe the same range the same way.
 */
struct extent_t {

    /** First byte of the range, which a disk requires to be a multiple of its offset alignment. */
    std::uint64_t offset = 0;

    /** Bytes in the range, which a disk requires to be a multiple of its length granularity. */
    std::uint64_t length = 0;

    /** One past the last byte, saturating rather than wrapping on a range that overflows. */
    [[nodiscard]] constexpr std::uint64_t end() const noexcept {
        return offset > UINT64_MAX - length ? UINT64_MAX : offset + length;
    }

    /** Whether the range covers no bytes, which only a flush submits. */
    [[nodiscard]] constexpr bool empty() const noexcept { return length == 0; }

    /** Whether the whole range sits below @p capacity, computed without overflowing. */
    [[nodiscard]] constexpr bool fits_within(std::uint64_t capacity) const noexcept {
        return offset <= capacity && length <= capacity - offset;
    }

    /** Whether @p other sits entirely inside this range. */
    [[nodiscard]] constexpr bool contains(extent_t const& other) const noexcept {
        return other.offset >= offset && other.end() <= end();
    }

    /** Whether the two ranges share at least one byte. */
    [[nodiscard]] constexpr bool overlaps(extent_t const& other) const noexcept {
        return offset < other.end() && other.offset < end();
    }

    /** Whether the offset and the length are both multiples of @p alignment. */
    [[nodiscard]] constexpr bool aligned_to(std::uint64_t alignment) const noexcept {
        return (offset & (alignment - 1)) == 0 && (length & (alignment - 1)) == 0;
    }

    /** The part of this range that sits below @p capacity, empty when it starts past the end. */
    [[nodiscard]] constexpr extent_t clipped_to(std::uint64_t capacity) const noexcept {
        if (offset >= capacity) return extent_t {offset, 0};
        std::uint64_t const reachable = capacity - offset;
        return extent_t {offset, length < reachable ? length : reachable};
    }

    friend constexpr bool operator==(extent_t const& first, extent_t const& second) noexcept {
        return first.offset == second.offset && first.length == second.length;
    }
};

#pragma endregion Extent

#pragma endregion Extent

#pragma region Ring

#pragma region Vocabulary

/** A table's version, raised on every change so a ring knows its copy is behind. */
using epoch_t = std::int32_t;

/** The hash of a thread's identity, which binds a queue pair to the thread that submits on it. */
using thread_identity_t = std::uint64_t;

/** Which reissue of a queue pair slot a token belongs to, the 16 bits of the token beside the tag. */
using queue_pair_generation_t = std::uint16_t;

/** The 32 bits of a token its queue pair's holder chooses, usually a slot index. */
using request_tag_t = std::uint32_t;

/** Requests one queue pair may keep outstanding. */
using queue_depth_t = std::uint32_t;

/** What a submitted request asks the device to do. */
enum class request_kind_t : std::uint8_t {

    /** Moves @c extent.length bytes from the volume into the registered buffer. */
    read_k,

    /** Moves @c extent.length bytes from the registered buffer onto the volume. */
    write_k,

    /** Makes every write this queue pair has already completed durable; its extent is empty. */
    flush_k,
};

/**
 *  @brief Whether the next request of the same batch waits for this one.
 *
 *  Chaining is the only ordering the ring offers, and it costs what @c IOSQE_IO_LINK costs: a short
 *  or failed result cuts the rest of the chain, which arrives as @c cancelled_k. A chain orders a
 *  bounded sequence inside one submitted batch and never orders across batches or across queue_pairs.
 */
enum class request_chaining_t : std::uint8_t {

    /** The request may start, execute and complete in any order against its neighbours. */
    independent_k,

    /** The following request of the same batch starts only after this one transfers in full. */
    chained_k,
};

/** How a request ended, which is the state the queue pair branches on rather than the status alone. */
enum class completion_state_t : std::uint8_t {

    /** Every byte of the extent moved, and the status is a success. */
    completed_k,

    /** Fewer bytes than the extent asked for moved, and only that prefix is data. */
    partial_k,

    /** The queue pair cancelled before the request started, or a chain ahead of it was cut. */
    cancelled_k,

    /** Nothing usable moved, and the status says why. */
    failed_k,

    /** More completions follow for this token, so the token is not retired yet. */
    continued_k,
};

/**
 *  @brief Where a registered buffer lives, which is a property of the registered memory and never of a request.
 *
 *  A compaction read into device memory and a compaction read into host memory are one request
 *  shape; only the registered memory call differs - @c cuFileBufRegister against
 *  @c IORING_REGISTER_BUFFERS against @c spdk_mem_register - so a disk that cannot reach device
 *  memory refuses at registered memory, where the caller can still choose another buffer.
 */
enum class memory_space_t : std::uint8_t {

    /** Ordinary host memory, which every transport reaches. */
    host_k,

    /** GPU memory, which only a GPUDirect-capable disk registers. */
    gpu_k,
};

/** One queue pair on a share of the ring's depth, or @c unallocated_k for a handle that holds none. */
enum class queue_pair_id_t : std::uint16_t {
    unallocated_k = 0xFFFF,
};

/** One registered memory range, or @c unregistered_k for a handle that names none. */
enum class registered_memory_id_t : std::uint16_t {
    unregistered_k = 0xFFFF,
};

/** One registered medium the bytes move to or from, or @c unregistered_k for a handle that names none. */
enum class registered_medium_id_t : std::uint16_t {
    unregistered_k = 0xFFFF,
};

/** What a caller may do to a medium, which the transport fixes when it registers one. */
enum class medium_access_t : std::uint8_t {

    /** Reads alone, so a write against this medium fails rather than growing it. */
    read_only_k,

    /** Reads and writes against a medium that must already exist. */
    read_write_k,

    /** Reads and writes, creating the medium at the requested length when it is absent. */
    create_read_write_k,
};

/**
 *  @brief What a caller asks a transport to register as a medium.
 *
 *  One shape serves all three transports because each reads only what it can: a file disk opens
 *  @c name as a path, a RAM disk keeps a buffer of @c length under that label, and a namespace
 *  ignores both, since the medium it registers is the namespace it was mounted on.
 */
struct medium_options_t {

    /** A path for a file disk, a label for a RAM disk, ignored by a namespace. */
    char const* name = nullptr;

    /** Bytes to create the medium with when it is absent, or zero to take whatever is there. */
    std::uint64_t length = 0;

    /** What the caller may do to it once it is registered. */
    medium_access_t access = medium_access_t::read_write_k;
};

#pragma endregion Vocabulary

#pragma region Tokens

/**
 *  @brief The 64 bits that travel to the device and back, naming the queue pair and the queue pair's request.
 *
 *  io_uring hands @c user_data back verbatim and inspects nothing, so the layout is ours: the queue pair
 *  identifier and its queue pair generation let the ring route a completion to exactly one queue pair, and the
 *  tag is the queue pair's own, usually an index into its request table. The generation matters because a
 *  released queue pair is reissued, and a stale token must not deliver into its successor's queue.
 *
 *  @note A tag is unique among one queue pair's in-flight requests, since cancellation on a real ring
 *      matches on these bits and would otherwise cancel the wrong request.
 */
struct request_token_t {

    /** The queue pair in the high 16 bits, the queue pair generation in the next 16, the queue pair's tag in the
     *  low 32. */
    std::uint64_t bits = 0;

    /** Packs a queue pair, its queue pair generation and the queue pair's own tag into one token. */
    [[nodiscard]] static constexpr request_token_t make(queue_pair_id_t queue_pair, queue_pair_generation_t generation,
                                                        request_tag_t tag) noexcept {
        return request_token_t {(static_cast<std::uint64_t>(queue_pair) << 48) |
                                (static_cast<std::uint64_t>(generation) << 32) | tag};
    }

    /** The queue pair this completion belongs to. */
    [[nodiscard]] constexpr queue_pair_id_t queue_pair() const noexcept {
        return static_cast<queue_pair_id_t>(static_cast<std::uint16_t>(bits >> 48));
    }

    /** The queue pair generation, which separates a queue pair from its predecessor in the same slot. */
    [[nodiscard]] constexpr queue_pair_generation_t generation() const noexcept {
        return static_cast<queue_pair_generation_t>(bits >> 32);
    }

    /** The queue pair's own tag, which the ring never interprets. */
    [[nodiscard]] constexpr request_tag_t tag() const noexcept { return static_cast<request_tag_t>(bits); }

    friend constexpr bool operator==(request_token_t first, request_token_t second) noexcept {
        return first.bits == second.bits;
    }
};

#pragma endregion Tokens

#pragma region Requests

/**
 *  @brief A place inside one registered range, which is how a request names its buffer.
 *
 *  A bare pointer cannot be submitted: io_uring's fixed forms want a slot index plus an offset,
 *  cuFile wants the registered base plus an offset, and SPDK needs the range in its memory map
 *  before @c spdk_vtophys can translate it. Naming the registered memory also makes the borrow visible,
 *  so a disk can refuse to deregister memory that is still a live DMA medium.
 */
struct memory_reference_t {

    /** The range the bytes sit in, which the queue pair obtained from @c register_memory. */
    registered_memory_id_t memory_id = registered_memory_id_t::unregistered_k;

    /** The queue pair generation of that range, so a stale place resolves to nothing. */
    queue_pair_generation_t generation = 0;

    /** Bytes from the start of the range to the first byte this request touches. */
    std::uint64_t offset = 0;
};

/**
 *  @brief Which registered medium a request moves bytes against, named the way its buffer is.
 *
 *  Storage is addressed exactly as memory is, by a registered slot and an offset into it, because a
 *  path cannot be submitted any more than a pointer can: @c IORING_REGISTER_FILES wants a table
 *  index, and an SPDK namespace has no path at all. The offset lives in the request's extent, so
 *  what a place carries is the slot and the generation that separates it from its predecessor.
 */
struct medium_reference_t {

    /** The medium the bytes move against, which the queue pair obtained from @c register_medium. */
    registered_medium_id_t medium_id = registered_medium_id_t::unregistered_k;

    /** The queue pair generation of that medium, so a stale place resolves to nothing. */
    queue_pair_generation_t generation = 0;
};

/**
 *  @brief One command the queue pair submits, borrowing a registered buffer until its token retires.
 *
 *  The ring never copies the bytes and never owns them. From the moment @c submit accepts a request
 *  until its completion is completed, the registered range behind @p buffer belongs to the device: the
 *  kernel or the drive may write into it at any instant, so the queue pair may not free, move, resize or
 *  reuse it. That window is enforced where it can be - a range with work in flight cannot be
 *  deregistered - which is why a request names a registered memory rather than a pointer.
 */
struct request_t {

    /** The registered medium the bytes move to or from, which one disk may hold many of. */
    medium_reference_t medium;

    /** The range inside that medium, empty for a flush. */
    extent_t extent;

    /** Where the bytes are read into, or written from; ignored by a flush. */
    memory_reference_t buffer;

    /** What the device is asked to do. */
    request_kind_t kind = request_kind_t::read_k;

    /** Whether the next request of the same batch waits for this one. */
    request_chaining_t chaining = request_chaining_t::independent_k;

    /** The bits that come back on the completion, minted by the queue pair. */
    request_token_t token;
};

/**
 *  @brief How one request ended, delivered only to the queue pair named in its token.
 *
 *  A partial transfer is a state and not a failure: a positioned read that meets the end of a
 *  volume, and a write that meets a full one, both move a prefix and stop. The ring guarantees the
 *  first @p transferred bytes of the buffer and nothing else: what lies past them is unspecified,
 *  since an @c O_DIRECT read zeroes the part of its range that has no volume behind it while a
 *  buffered one leaves those bytes alone. A caller that needs the whole extent tests @c whole and
 *  reissues the remainder, and a block layer treats anything short of the whole block as a refusal.
 */
struct completion_t {

    /** The token of the request this completion belongs to. */
    request_token_t token;

    /** Bytes actually moved, which is the length of the buffer prefix the device touched. */
    std::uint64_t transferred_bytes = 0;

    /** Why it ended this way, a success for @c completed_k and @c partial_k alike. */
    status_t status;

    /** How it ended, which the queue pair branches on. */
    completion_state_t state = completion_state_t::completed_k;

    /** Whether the whole extent moved, which is the only state a block read may trust. */
    [[nodiscard]] bool completed_in_full() const noexcept { return state == completion_state_t::completed_k; }

    /** Whether this completion retires the token, which a multishot transport's interim ones do not. */
    [[nodiscard]] bool retires() const noexcept { return state != completion_state_t::continued_k; }
};

/** A read of @p extent of @p medium into @p buffer, tagged with @p token. */
[[nodiscard]] constexpr request_t read_request(medium_reference_t medium, extent_t extent, memory_reference_t buffer,
                                               request_token_t token) noexcept {
    return request_t {medium, extent, buffer, request_kind_t::read_k, request_chaining_t::independent_k, token};
}

/** A write of @p extent of @p medium from @p buffer, tagged with @p token. */
[[nodiscard]] constexpr request_t write_request(medium_reference_t medium, extent_t extent, memory_reference_t buffer,
                                                request_token_t token) noexcept {
    return request_t {medium, extent, buffer, request_kind_t::write_k, request_chaining_t::independent_k, token};
}

/** A flush of everything this queue pair has already completed of @p medium, tagged with @p token. */
[[nodiscard]] constexpr request_t flush_request(medium_reference_t medium, request_token_t token) noexcept {
    return request_t {
        medium, extent_t {}, memory_reference_t {}, request_kind_t::flush_k, request_chaining_t::independent_k, token};
}

#pragma endregion Requests

#pragma region Queue Pairs

/**
 *  @brief One queue pair on a share of the ring's depth, held by exactly one thread at a time.
 *
 *  The queue pair is the queue pair identity the transports demand, not a convenience: an io_uring ring set up
 *  with @c SINGLE_ISSUER refuses a foreign reaper, and an SPDK queue pair corrupts itself silently
 *  under one. Moving a queue pair hands it to another thread, which is the only legal handoff when a
 *  controller offers fewer queue pairs than the executor has threads; a queue pair is never shared. The
 *  receiving thread announces itself with the disk's @c adopt, since a ring cannot observe a move and
 *  an unannounced one is indistinguishable from two threads sharing a queue pair.
 *
 *  @note Return the queue pair with the disk's @c release before the handle dies, since the handle cannot
 *      drain the device on its own.
 */
class queue_pair_t {
  public:

    queue_pair_t() noexcept = default;
    queue_pair_t(queue_pair_t const&) = delete;
    queue_pair_t& operator=(queue_pair_t const&) = delete;

    queue_pair_t(queue_pair_t&& other) noexcept
        : id_(std::exchange(other.id_, queue_pair_id_t::unallocated_k)),
          generation_(std::exchange(other.generation_, 0)), depth_(std::exchange(other.depth_, 0)) {}

    queue_pair_t& operator=(queue_pair_t&& other) noexcept {
        assert(!held() && "Release a queue pair before overwriting it");
        id_ = std::exchange(other.id_, queue_pair_id_t::unallocated_k);
        generation_ = std::exchange(other.generation_, 0);
        depth_ = std::exchange(other.depth_, 0);
        return *this;
    }

    ~queue_pair_t() noexcept { assert(!held() && "Release a queue pair before destroying it"); }

    /** Builds the handle a disk hands back from its @c open; callers never call this. */
    [[nodiscard]] static queue_pair_t leased(queue_pair_id_t id, queue_pair_generation_t generation,
                                             queue_depth_t depth) noexcept {
        queue_pair_t queue_pair;
        queue_pair.id_ = id;
        queue_pair.generation_ = generation;
        queue_pair.depth_ = depth;
        return queue_pair;
    }

    /** Empties the handle once the disk has taken the queue pair back; callers never call this. */
    void surrender() noexcept {
        id_ = queue_pair_id_t::unallocated_k;
        generation_ = 0;
        depth_ = 0;
    }

    /** Whether this handle still holds a queue pair. */
    [[nodiscard]] bool held() const noexcept { return id_ != queue_pair_id_t::unallocated_k; }

    /** The slot this queue pair occupies. */
    [[nodiscard]] queue_pair_id_t id() const noexcept { return id_; }

    /** The generation that separates this queue pair from the slot's previous one. */
    [[nodiscard]] queue_pair_generation_t generation() const noexcept { return generation_; }

    /** Requests this queue pair may keep in flight at once, reserved when the queue pair was taken. */
    [[nodiscard]] queue_depth_t depth() const noexcept { return depth_; }

    /** Mints a token of this queue pair carrying the queue pair's own @p tag. */
    [[nodiscard]] request_token_t token(request_tag_t tag) const noexcept {
        return request_token_t::make(id_, generation_, tag);
    }

  private:

    /** The slot the queue pair occupies, @c unallocated_k once surrendered. */
    queue_pair_id_t id_ = queue_pair_id_t::unallocated_k;

    /** The generation of this queue pair, raised every time the slot is leased again. */
    queue_pair_generation_t generation_ = 0;

    /** Requests this queue pair may keep in flight at once. */
    queue_depth_t depth_ = 0;
};

/**
 *  @brief One registered memory range, which a request addresses by slot and offset.
 *
 *  A registered memory is the borrow the ring can enforce: while requests against it are in flight the
 *  disk refuses to give it back, so a buffer cannot be freed under a live DMA medium by accident.
 *  Registration is also where a transport says what it cannot reach, such as device memory on the
 *  blocking floor, and where it pins pages once instead of per request.
 */
class registered_memory_t {
  public:

    registered_memory_t() noexcept = default;
    registered_memory_t(registered_memory_t const&) = delete;
    registered_memory_t& operator=(registered_memory_t const&) = delete;

    registered_memory_t(registered_memory_t&& other) noexcept
        : id_(std::exchange(other.id_, registered_memory_id_t::unregistered_k)),
          generation_(std::exchange(other.generation_, 0)), memory_(std::exchange(other.memory_, std::span<byte_t> {})),
          space_(other.space_) {}

    registered_memory_t& operator=(registered_memory_t&& other) noexcept {
        assert(!held() && "Deregister a range before overwriting its handle");
        id_ = std::exchange(other.id_, registered_memory_id_t::unregistered_k);
        generation_ = std::exchange(other.generation_, 0);
        memory_ = std::exchange(other.memory_, std::span<byte_t> {});
        space_ = other.space_;
        return *this;
    }

    ~registered_memory_t() noexcept { assert(!held() && "Deregister a range before destroying its handle"); }

    /** Builds the handle a disk hands back from its @c register_memory; callers never call this. */
    [[nodiscard]] static registered_memory_t registered(registered_memory_id_t id, queue_pair_generation_t generation,
                                                        std::span<byte_t> memory, memory_space_t space) noexcept {
        registered_memory_t registered_memory;
        registered_memory.id_ = id;
        registered_memory.generation_ = generation;
        registered_memory.memory_ = memory;
        registered_memory.space_ = space;
        return registered_memory;
    }

    /** Empties the handle once the disk has taken the range back; callers never call this. */
    void surrender() noexcept {
        id_ = registered_memory_id_t::unregistered_k;
        generation_ = 0;
        memory_ = std::span<byte_t> {};
    }

    /** Whether this handle still names a registered range. */
    [[nodiscard]] bool held() const noexcept { return id_ != registered_memory_id_t::unregistered_k; }

    /** The slot the range occupies. */
    [[nodiscard]] registered_memory_id_t id() const noexcept { return id_; }

    /** The generation that separates this range from the slot's previous one. */
    [[nodiscard]] queue_pair_generation_t generation() const noexcept { return generation_; }

    /** The memory the range covers, which the queue pair allocated and still owns. */
    [[nodiscard]] std::span<byte_t> bytes() const noexcept { return memory_; }

    /** Where the range lives, which the disk agreed to at registered_memory. */
    [[nodiscard]] memory_space_t space() const noexcept { return space_; }

    /** The place @p offset bytes into this range, which a request carries instead of a pointer. */
    [[nodiscard]] memory_reference_t reference(std::uint64_t offset = 0) const noexcept {
        return memory_reference_t {id_, generation_, offset};
    }

  private:

    /** The slot the range occupies, @c unregistered_k once surrendered. */
    registered_memory_id_t id_ = registered_memory_id_t::unregistered_k;

    /** The generation of this registered memory, raised every time the slot is used again. */
    queue_pair_generation_t generation_ = 0;

    /** The memory the range covers, owned by the caller throughout. */
    std::span<byte_t> memory_;

    /** Where that memory lives. */
    memory_space_t space_ = memory_space_t::host_k;
};

/**
 *  @brief One registered medium, which a request addresses by slot rather than by name.
 *
 *  A medium is registered for the same reason a buffer is: @c IORING_REGISTER_FILES addresses a file
 *  by table index and skips the registered medium lookup every request would otherwise repeat, and an SPDK
 *  namespace has no name to pass at all. Registering it also makes the borrow visible, so a disk can
 *  refuse to close a medium that requests are still in flight against.
 */
class registered_medium_t {
  public:

    registered_medium_t() noexcept = default;
    registered_medium_t(registered_medium_t const&) = delete;
    registered_medium_t& operator=(registered_medium_t const&) = delete;

    registered_medium_t(registered_medium_t&& other) noexcept
        : id_(std::exchange(other.id_, registered_medium_id_t::unregistered_k)),
          generation_(std::exchange(other.generation_, 0)), capacity_(std::exchange(other.capacity_, 0)) {}

    registered_medium_t& operator=(registered_medium_t&& other) noexcept {
        assert(!held() && "Deregister a medium before overwriting its handle");
        id_ = std::exchange(other.id_, registered_medium_id_t::unregistered_k);
        generation_ = std::exchange(other.generation_, 0);
        capacity_ = std::exchange(other.capacity_, 0);
        return *this;
    }

    ~registered_medium_t() noexcept { assert(!held() && "Deregister a medium before destroying its handle"); }

    /** Builds the handle a disk hands back from its @c register_medium; callers never call this. */
    [[nodiscard]] static registered_medium_t registered(registered_medium_id_t id, queue_pair_generation_t generation,
                                                        std::uint64_t capacity) noexcept {
        registered_medium_t registered_medium;
        registered_medium.id_ = id;
        registered_medium.generation_ = generation;
        registered_medium.capacity_ = capacity;
        return registered_medium;
    }

    /** Empties the handle once the disk has taken the medium back; callers never call this. */
    void surrender() noexcept {
        id_ = registered_medium_id_t::unregistered_k;
        generation_ = 0;
        capacity_ = 0;
    }

    /** Whether this handle still names a registered medium. */
    [[nodiscard]] bool held() const noexcept { return id_ != registered_medium_id_t::unregistered_k; }

    /** The slot the medium occupies. */
    [[nodiscard]] registered_medium_id_t id() const noexcept { return id_; }

    /** The generation that separates this medium from the slot's previous one. */
    [[nodiscard]] queue_pair_generation_t generation() const noexcept { return generation_; }

    /** Bytes the medium spans, which an extent past it reads short or fails. */
    [[nodiscard]] std::uint64_t capacity() const noexcept { return capacity_; }

    /** The place a request carries instead of a name. */
    [[nodiscard]] medium_reference_t reference() const noexcept { return medium_reference_t {id_, generation_}; }

  private:

    /** The slot the medium occupies, @c unregistered_k once surrendered. */
    registered_medium_id_t id_ = registered_medium_id_t::unregistered_k;

    /** The generation of this registered memory, raised every time the slot is used again. */
    queue_pair_generation_t generation_ = 0;

    /** Bytes the medium spans, as the transport reported them at registered_memory. */
    std::uint64_t capacity_ = 0;
};

#pragma endregion Queue Pairs

#pragma region Capabilities

/**
 *  @brief What one disk accepts, probed at mount and never assumed.
 *
 *  The three alignments are genuinely different numbers: @c statx with @c STATX_DIOALIGN reports a
 *  memory alignment of 4 and an offset alignment of 512 on ext4 over a 512-byte-logical device,
 *  while a 4 Kn drive, SPDK's page-crossing rule and cuFile all want 4096. Aligning to
 *  @c portable_alignment_k satisfies every transport, so a caller that does not want to branch may
 *  use it and be accepted everywhere.
 */
struct ring_capabilities_t {

    /** Alignment the first byte of a registered buffer must meet. */
    std::uint64_t memory_alignment = 1;

    /** Alignment an extent's offset must meet. */
    std::uint64_t offset_alignment = 1;

    /** Multiple an extent's length must be. */
    std::uint64_t length_granularity = 1;

    /** Largest extent one request may carry. */
    std::uint64_t max_transfer_bytes = 0;

    /** Requests one queue pair may keep in flight at once. */
    queue_depth_t depth_limit = 0;

    /** Leases the ring can hand out at once, which a controller's queue-pair count bounds. */
    std::uint16_t queue_pairs_limit = 0;

    /** Targets the disk registers at once, which is one on a namespace and many in a directory. */
    std::uint16_t media_limit = 0;

    /** Whether the transport can register and address GPU memory. */
    bool reaches_gpu_memory = false;
};

#pragma endregion Capabilities

#pragma region Contract

/**
 *  @brief What every NVMe disk offers, so a caller compiles against the transport its mount chose.
 *
 *  Nothing here blocks. @c submit takes a prefix of the batch and answers how much it took, which is
 *  zero on a full ring; @c poll answers how many completions it moved, which is zero when none are
 *  ready. A bounded wait is a spin over @c poll, because SPDK has no interrupt at all and an
 *  io_uring ring with @c DEFER_TASKRUN only processes completions when its owning thread enters.
 */
template <typename disk_type_>
concept ring_concept = requires(disk_type_& disk, disk_type_ const& constant, queue_pair_t& queue_pair,
                                registered_memory_t& registered_memory, registered_medium_t& registered_medium,
                                medium_options_t medium, std::span<request_t const> requests,
                                std::span<completion_t> completions, std::span<byte_t> memory) {
    { disk.acquire_queue_pair(std::uint32_t {}) } -> std::same_as<expected<queue_pair_t>>;
    { disk.bind_queue_pair(queue_pair) } -> std::same_as<status_t>;
    { disk.release_queue_pair(queue_pair) } -> std::same_as<status_t>;
    { disk.register_memory(memory, memory_space_t::host_k) } -> std::same_as<expected<registered_memory_t>>;
    { disk.unregister_memory(registered_memory) } -> std::same_as<status_t>;
    { disk.register_medium(medium) } -> std::same_as<expected<registered_medium_t>>;
    { disk.unregister_medium(registered_medium) } -> std::same_as<status_t>;
    { disk.submit(queue_pair, requests) } -> std::same_as<expected<std::size_t>>;
    { disk.poll_completions(queue_pair, completions) } -> std::same_as<expected<std::size_t>>;
    { disk.cancel(queue_pair) } -> std::same_as<void>;
    { disk.drain(queue_pair) } -> std::same_as<status_t>;
    { constant.outstanding(queue_pair) } -> std::same_as<std::size_t>;
    { constant.capabilities() } -> std::same_as<ring_capabilities_t>;
};

/** Spins over a ring that never blocks, so a bounded wait needs no fiber and no interrupt. */
inline constexpr std::size_t poll_spin_limit_k = std::size_t {1} << 30;

/**
 *  @brief Reaps until @p wanted completions are in @p into, or the spin budget runs out.
 *  @param[in,out] disk The ring the queue pair polls from.
 *  @param[in] queue_pair The queue pair whose completions are wanted, held by the calling thread.
 *  @param[out] into Room for the completions, which bounds how many may be wanted.
 *  @param[in] wanted Completions to wait for, at most the size of @p into.
 *  @param[in] spins_limit Spins before the wait gives up, which a healthy ring never reaches.
 *  @return The completions moved into @p into, which is @p wanted unless the budget ran out.
 *  @tparam waiter_type_ What the polling core does after a poll that moved nothing.
 */
template <typename waiter_type_, typename disk_type_>
    requires ring_concept<disk_type_>
expected<std::size_t> poll_completions_at_least(disk_type_& disk, queue_pair_t const& queue_pair,
                                                std::span<completion_t> into, std::size_t wanted,
                                                std::size_t spins_limit = poll_spin_limit_k) noexcept {
    if (wanted > into.size())
        return status_t {status_code_t::invalid_argument_k, "A bounded wait needs room for what it wants"};
    std::size_t completed = 0;
    for (std::size_t spins = 0; completed < wanted && spins != spins_limit; ++spins) {
        auto [moved, poll_completions_status] = disk.poll_completions(queue_pair, into.subspan(completed));
        if (poll_completions_status.failed()) return poll_completions_status;
        if (moved == 0) waiter_type_ {}(&spins, spins, std::size_t {0});
        completed += moved;
    }
    return completed;
}

#pragma endregion Contract

#pragma endregion Ring

} // namespace unum::ustore::utilities
