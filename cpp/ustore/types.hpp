/**
 *  @file cpp/ustore/types.hpp
 *  @author Ash Vardanian
 *  @date July 4, 2022
 *  @brief The pieces every C++ wrapper shares: statuses, expected values and deferred calls,
 *      identifiers and value views, strided spans and tapes, locations, arenas, executors,
 *      receivers and progress handlers, adding no ABI of their own and throwing nothing.
 *
 *  Every verb is written once, as a template on its completion: a blocking receiver answers a bare
 *  @c status_t or an @c expected value, and a deferred one answers a @c deferred call its one owner
 *  polls, waits on or awaits. A strided layout advances by a byte stride instead of @c sizeof, so a
 *  zero stride repeats one value. A tape indexes variable-length values by a base pointer and N+1
 *  gapless offsets of 32 or 64 bits, as every call exports them into its arena, and any other run
 *  of strings reaches a call through an accessor.
 */
#pragma once

#include <climits> // `CHAR_BIT`
#include <cstddef> // `std::size_t`, `std::ptrdiff_t`, `std::byte`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstring> // `std::strlen`

#include <algorithm>   // `std::ranges::equal`, `std::lexicographical_compare_three_way`
#include <compare>     // `std::strong_ordering`
#include <concepts>    // `std::invocable`, `std::same_as`, `std::convertible_to`
#include <coroutine>   // `std::coroutine_handle`
#include <iterator>    // `std::random_access_iterator_tag`, `std::size`, `std::data`
#include <ranges>      // `std::ranges::contiguous_range`, `std::ranges::sized_range`, `std::ranges::size`
#include <span>        // `std::span`, `std::as_bytes`
#include <string_view> // `std::string_view`, `std::basic_string_view`
#include <type_traits> // `std::conditional_t`, `std::is_void_v`, `std::is_nothrow_invocable_r_v`
#include <utility>     // `std::exchange`, `std::move`, `std::swap`, `std::declval`

#include "ustore/ustore.h" // `ustore_outcome_t`, `ustore_outcome_wait`, `ustore_key_t` and every call

#pragma region Identifier Orders

/** Orders UUIDs as their bytes, which is how every engine stores and walks them. A template, so
 *  a translation unit that also declares plain comparisons of the same structs picks those. */
template <std::same_as<ustore_uuid_t> uuid_type_>
constexpr std::strong_ordering operator<=>(uuid_type_ const& first, uuid_type_ const& second) noexcept {
    return std::lexicographical_compare_three_way(first.bytes, first.bytes + 16, second.bytes, second.bytes + 16);
}

template <std::same_as<ustore_uuid_t> uuid_type_>
constexpr bool operator==(uuid_type_ const& first, uuid_type_ const& second) noexcept {
    return (first <=> second) == 0;
}

/** Orders keys and collection IDs as the UUIDs they wrap. */
template <typename identifier_type_>
    requires(std::same_as<identifier_type_, ustore_key_t> || std::same_as<identifier_type_, ustore_collection_id_t>)
constexpr std::strong_ordering operator<=>(identifier_type_ const& first, identifier_type_ const& second) noexcept {
    return first.uuid <=> second.uuid;
}

template <typename identifier_type_>
    requires(std::same_as<identifier_type_, ustore_key_t> || std::same_as<identifier_type_, ustore_collection_id_t>)
constexpr bool operator==(identifier_type_ const& first, identifier_type_ const& second) noexcept {
    return first.uuid == second.uuid;
}

#pragma endregion Identifier Orders

namespace unum::ustore {

/** What went wrong, with one enumerator per status the library reports. */
enum class status_code_t : int {
    success_k = ustore_status_success_k,
    deferred_k = ustore_status_deferred_k,
    out_of_memory_k = ustore_status_out_of_memory_k,
    read_only_k = ustore_status_read_only_k,
    io_k = ustore_status_io_k,
    device_k = ustore_status_device_k,
    malformed_k = ustore_status_malformed_k,
    internal_k = ustore_status_internal_k,
    overflow_k = ustore_status_overflow_k,
    uninitialized_k = ustore_status_uninitialized_k,
    config_mismatch_k = ustore_status_config_mismatch_k,
    contended_k = ustore_status_contended_k,
    cancelled_k = ustore_status_cancelled_k,
    unsupported_k = ustore_status_unsupported_k,
    invalid_argument_k = ustore_status_invalid_argument_k,
    not_found_k = ustore_status_not_found_k,
    already_exists_k = ustore_status_already_exists_k,
    write_conflict_k = ustore_status_write_conflict_k,
    read_conflict_k = ustore_status_read_conflict_k,
    phantom_conflict_k = ustore_status_phantom_conflict_k,
    capacity_exhausted_k = ustore_status_capacity_exhausted_k,
};

#pragma region Shared Core

/** Why a call failed: a code recovery branches on, and an optional static note naming the broken
 *  invariant, step or resource, for logs and regression analysis. */
struct [[nodiscard]] status_t {
    status_code_t code = status_code_t::success_k;
    char const* note = nullptr;

    constexpr status_t() noexcept = default;
    constexpr status_t(status_code_t reason, char const* detail = nullptr) noexcept : code(reason), note(detail) {}
    constexpr bool succeeded() const noexcept { return code == status_code_t::success_k; }
    constexpr bool failed() const noexcept { return code != status_code_t::success_k; }
    friend constexpr bool operator==(status_t const& status, status_code_t code) noexcept {
        return status.code == code;
    }
};

static_assert(sizeof(status_t) == 2 * sizeof(void*) && std::is_trivially_copyable_v<status_t>,
              "a status is a code and a note pointer, returned in two registers");

/** A call's value beside its status, bound natively as <tt>auto [value, status]</tt>. */
template <typename value_type_>
struct [[nodiscard]] expected {
    value_type_ value {};
    status_t status {};
};

/** Where a call writes what @p value_type_ is made of. */
template <typename value_type_>
struct outputs {

    /** The answer, which the call writes in place. */
    value_type_ value {};

    value_type_ answer() const noexcept { return value; }
};

/** A call that outputs nothing but its status. */
template <>
struct outputs<void> {};

/** Runs @p poll with @p state later on its own loop, storing the two words and allocating
 *  nothing. */
template <typename scheduler_type_>
concept a_scheduler = requires(scheduler_type_& loop, void (*poll)(void*) noexcept, void* state) {
    loop.post(poll, state);
};

template <typename value_type_, a_scheduler scheduler_type_>
class awaiter;

/**
 *  @brief A submitted call: its outcome and its outputs, owned until the call is final, while the
 *      receiver, inputs, arena and handlers it was handed stay allocated and unchanged until then.
 *
 *  It never moves, as the library writes into it, and its one owner polls @c status, blocks in
 *  @c wait or awaits @c wait on its own loop; destruction waits, so nothing the call writes into
 *  goes away under it.
 */
template <typename value_type_ = void>
class [[nodiscard]] deferred {
    ustore_outcome_t outcome_ {};
    outputs<value_type_> outputs_ {};

  public:
    /** What @c wait answers: what the blocking call would have. */
    using answer_type = std::conditional_t<std::is_void_v<value_type_>, status_t, expected<value_type_>>;

    /** An empty slot, final from the start, which @c resubmit fills. */
    deferred() noexcept = default;

    /** Runs @p submit with this call's outcome and outputs, where the library reports. */
    template <typename submit_type_>
        requires std::invocable<submit_type_ const&, ustore_outcome_t*, outputs<value_type_>*>
    explicit deferred(submit_type_ const& submit) noexcept {
        submit(&outcome_, &outputs_);
    }

    /** Waits only while the slot's previous call still runs, then runs @p submit in its place. */
    template <typename submit_type_>
        requires std::invocable<submit_type_ const&, ustore_outcome_t*, outputs<value_type_>*>
    void resubmit(submit_type_ const& submit) noexcept {
        ustore_outcome_wait(&outcome_);
        outcome_ = {};
        outputs_ = {};
        submit(&outcome_, &outputs_);
    }

    /** A call refused before it crossed, final from the start. */
    explicit deferred(status_t status) noexcept : outcome_ {static_cast<ustore_status_t>(status.code), status.note} {}

    deferred(deferred const&) = delete;
    deferred& operator=(deferred const&) = delete;
    ~deferred() noexcept { ustore_outcome_wait(&outcome_); }

    /** @c status_code_t::deferred_k until the call is final, then its final status, polled without
     *  blocking. */
    status_code_t status() noexcept { return static_cast<status_code_t>(ustore_outcome_status(&outcome_)); }

    /** Blocks until the call is final and answers what a blocking call would have. */
    answer_type wait() noexcept {
        ustore_outcome_wait(&outcome_);
        status_t const status {static_cast<status_code_t>(outcome_.status), outcome_.message};
        if constexpr (std::is_void_v<value_type_>) return status;
        else if (status.failed()) return {{}, status};
        else return {outputs_.answer()};
    }

    /** Awaits the call on @p loop, which polls its status until it is final. */
    template <a_scheduler scheduler_type_>
    awaiter<value_type_, scheduler_type_> wait(scheduler_type_& loop) noexcept {
        return {*this, loop};
    }
};

/** A @c deferred being awaited, living in the awaiting coroutine's frame: each poll reads the
 *  status word, and one not yet final posts itself again. */
template <typename value_type_, a_scheduler scheduler_type_>
class awaiter {
    deferred<value_type_>& call_;
    scheduler_type_& loop_;
    std::coroutine_handle<> caller_ {};

    static void poll_(void* state) noexcept {
        awaiter& awaited = *static_cast<awaiter*>(state);
        if (awaited.await_ready()) return awaited.caller_.resume();
        awaited.loop_.post(&poll_, state);
    }

  public:
    awaiter(deferred<value_type_>& call, scheduler_type_& loop) noexcept : call_(call), loop_(loop) {}
    bool await_ready() noexcept { return call_.status() != status_code_t::deferred_k; }
    void await_suspend(std::coroutine_handle<> caller) noexcept {
        caller_ = caller;
        loop_.post(&poll_, this);
    }
    typename deferred<value_type_>::answer_type await_resume() noexcept { return call_.wait(); }
};

/** Whether @p answer_type_ is a call the caller holds rather than a final answer. */
template <typename answer_type_>
inline constexpr bool is_deferred = false;
template <typename value_type_>
inline constexpr bool is_deferred<deferred<value_type_>> = true;

/** What a final @p answer_type_ carries beside its status, @c void for a bare @c status_t. */
template <typename answer_type_>
struct answer_value {
    using type = void;
};
template <typename value_type_>
struct answer_value<expected<value_type_>> {
    using type = value_type_;
};
template <typename answer_type_>
using answer_value_t = typename answer_value<answer_type_>::type;

/** Answers @p answer_type_ from a verb's @p body: a deferred call passes C its own outcome and
 *  outputs; a blocking call writes its outputs in place, then takes the C outcome's two words once
 *  it is final. */
template <typename answer_type_, typename body_type_>
answer_type_ answered_(body_type_ const& body) noexcept {
    if constexpr (is_deferred<answer_type_>) return answer_type_(body);
    else {
        using value_type = answer_value_t<answer_type_>;
        ustore_outcome_t reported {};
        outputs<value_type> written {};
        body(&reported, &written);
        status_t const status {static_cast<status_code_t>(reported.status), reported.message};
        if constexpr (std::is_void_v<value_type>) return status;
        else if (status.failed()) return {{}, status};
        else return {written.answer()};
    }
}

/** Answers a call refused before it crossed, final from the start. */
template <typename answer_type_>
answer_type_ answered_(status_t refused) noexcept {
    if constexpr (is_deferred<answer_type_> || std::is_same_v<answer_type_, status_t>) return answer_type_(refused);
    else return {{}, refused};
}

/** Owns one handle of @p handle_type_, which @p free_ releases, moving it and never copying it. */
template <typename handle_type_, void (*free_)(handle_type_)>
class owner {
    handle_type_ handle_ = nullptr;

  public:
    owner() noexcept = default;
    owner(owner&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    owner& operator=(owner&& other) noexcept {
        std::swap(handle_, other.handle_);
        return *this;
    }
    owner(owner const&) = delete;
    owner& operator=(owner const&) = delete;
    ~owner() noexcept { free_(handle_); }

    handle_type_ handle() const noexcept { return handle_; }

    /** The output field a lifecycle verb writes the new handle into. */
    handle_type_* handle_field() noexcept { return &handle_; }
};

/** Creates an @p owner_type_ through @p verb, which writes the new handle into @p field of
 *  @p arguments. */
template <typename owner_type_, typename arguments_type_, typename handle_type_>
expected<owner_type_> opened_(void (*verb)(arguments_type_*), arguments_type_& arguments,
                              handle_type_* arguments_type_::* field) noexcept {
    expected<owner_type_> opened;
    arguments.*field = opened.value.handle_field();
    opened.status = answered_<status_t>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        arguments.outcome = outcome;
        verb(&arguments);
    });
    return opened;
}

#pragma endregion Shared Core

/** A blocking call's answer, a bare status for void verbs, or a deferred call the caller holds. */
template <typename value_type_ = void, ustore_completion_t completion_ = ustore_completion_blocking_k>
using deferrable = std::conditional_t<completion_ == ustore_completion_deferred_k, deferred<value_type_>,
                                      std::conditional_t<std::is_void_v<value_type_>, status_t, expected<value_type_>>>;

/** A handle owned, moved and never copied, whose verbs block on the calling thread. */
template <typename handle_type_, void (*free_)(handle_type_)>
struct owning {
    static constexpr ustore_completion_t completion = ustore_completion_blocking_k;
    owner<handle_type_, free_> owner_;
    handle_type_ handle() const noexcept { return owner_.handle(); }
    ustore_executor_t executor() const noexcept { return nullptr; }
    handle_type_* handle_field() noexcept { return owner_.handle_field(); }
};

/** A handle the caller owns and the executor its verbs run on, completing as @p completion_
 *  says. */
template <typename handle_type_, ustore_completion_t completion_>
struct caller_owned {
    static constexpr ustore_completion_t completion = completion_;
    handle_type_ handle_ = nullptr;
    ustore_executor_t executor_ = nullptr;
    handle_type_ handle() const noexcept { return handle_; }
    ustore_executor_t executor() const noexcept { return executor_; }
};

/** Only an owning receiver opens, loads and hands out cursors. */
template <typename receiver_type_>
concept an_owning_receiver = requires(receiver_type_& receiver) { receiver.handle_field(); };

/** Whether a receiver completed as @p completion_ submits its calls and answers @c deferred ones,
 *  rather than blocking until each is final. */
template <ustore_completion_t completion_>
inline constexpr bool defers = completion_ == ustore_completion_deferred_k;

/** Whether a receiver completed as @p completion_ blocks until each call is final, the receivers
 *  every shorthand is written for. */
template <ustore_completion_t completion_>
inline constexpr bool blocks = !defers<completion_>;

#pragma region Identifiers

/** The UUID spelling @p integer big-endian in its last eight bytes, so UUIDs made from integers
 *  order as the integers. */
constexpr ustore_uuid_t uuid_from_integer(std::uint64_t integer) noexcept {
    ustore_uuid_t uuid {};
    for (unsigned index = 0; index != 8; ++index)
        uuid.bytes[15 - index] = static_cast<std::uint8_t>(integer >> (8 * index));
    return uuid;
}

/** The key spelling @p integer, as @c uuid_from_integer does. */
constexpr ustore_key_t key_from_integer(std::uint64_t integer) noexcept { return {uuid_from_integer(integer)}; }

/** The first key in their order, the nil UUID, where a walk of a whole collection starts. */
inline constexpr ustore_key_t smallest_key_k {};

/** The last key in their order, every bit set, which @c ustore_key_unknown_k spells too. */
inline constexpr ustore_key_t largest_key_k {
    {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}}};

/** The key right after @p key in their order, where a scan resuming past @p key starts; the last
 *  key has none and wraps to the first. */
constexpr ustore_key_t key_after(ustore_key_t key) noexcept {
    for (unsigned index = 16; index != 0; --index)
        if (++key.uuid.bytes[index - 1] != 0) break;
    return key;
}

/** A key inside a collection, laid out so a strided pointer can address either member. */
struct collection_key_t {

    /** Collection owning the key. */
    ustore_collection_id_t collection {};

    /** Key inside the collection. */
    ustore_key_t key {};

    collection_key_t() = default;
    explicit collection_key_t(ustore_key_t key) noexcept : key(key) {}
    collection_key_t(ustore_collection_id_t collection, ustore_key_t key) noexcept : collection(collection), key(key) {}

    /** The same key addressed inside @p other_collection. */
    collection_key_t in(ustore_collection_id_t other_collection) const noexcept { return {other_collection, key}; }

    friend bool operator==(collection_key_t const&, collection_key_t const&) = default;
    friend std::strong_ordering operator<=>(collection_key_t const&, collection_key_t const&) = default;
};

/** A graph edge, or a relationship in DBMS terms. */
struct edge_t {

    /** Vertex the edge leaves. */
    ustore_key_t source;

    /** Vertex the edge enters. */
    ustore_key_t target;

    /** Identifier of the edge itself. */
    ustore_key_t id {ustore_edge_id_default_k};

    friend bool operator==(edge_t const&, edge_t const&) = default;
};

#pragma endregion Identifiers

#pragma region Value Views

/** The length a missing value carries, which no stored value reaches, so a view tells it apart from
 *  an empty one. */
inline constexpr ustore_length_t missing_length_k = ~ustore_length_t {0};

/** A view of a stored value that tells a missing value apart from an empty one. */
class value_view_t {

    ustore_byte_t const* pointer_ = nullptr;
    ustore_length_t length_ = missing_length_k;

  public:
    using value_type = std::byte;
    using iterator_t = std::byte const*;

    value_view_t() = default;
    value_view_t(ustore_byte_t const* pointer, ustore_length_t length) noexcept : pointer_(pointer), length_(length) {}

    /** Views the bytes of @p bytes, a value that is always present. */
    value_view_t(ustore_bytes_view_t bytes) noexcept : pointer_(bytes.data), length_(bytes.length) {}

    value_view_t(std::byte const* begin, std::byte const* end) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(begin)), length_(static_cast<ustore_length_t>(end - begin)) {}

    value_view_t(std::byte const* begin, std::size_t length) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(begin)), length_(static_cast<ustore_length_t>(length)) {}

    /** Views @p bytes, a value that is always present, an empty one included. */
    value_view_t(std::span<std::byte const> bytes) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(bytes.data())),
          length_(static_cast<ustore_length_t>(bytes.size())) {}

    value_view_t(char const* string) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(string)),
          length_(static_cast<ustore_length_t>(std::strlen(string))) {}

    value_view_t(char const* string, std::size_t length) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(string)), length_(static_cast<ustore_length_t>(length)) {}

    template <typename char_type_, typename traits_type_>
    value_view_t(std::basic_string_view<char_type_, traits_type_> view) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(view.data())),
          length_(static_cast<ustore_length_t>(view.size() * sizeof(char_type_))) {}

    value_view_t(std::string_view view) noexcept
        : pointer_(reinterpret_cast<ustore_byte_t const*>(view.data())),
          length_(static_cast<ustore_length_t>(view.size())) {}

    /** An empty value that is present, unlike a default-constructed missing one. */
    static value_view_t make_empty() noexcept { return value_view_t {static_cast<ustore_byte_t const*>(nullptr), 0}; }

    operator bool() const noexcept { return length_ != missing_length_k; }
    std::size_t size() const noexcept { return length_ == missing_length_k ? 0 : length_; }
    std::byte const* data() const noexcept {
        return length_ != missing_length_k ? reinterpret_cast<std::byte const*>(pointer_) : nullptr;
    }

    std::byte const* begin() const noexcept { return data(); }
    std::byte const* end() const noexcept { return data() + size(); }
    bool empty() const noexcept { return !size(); }
    operator std::string_view() const noexcept { return {reinterpret_cast<char const*>(pointer_), size()}; }

    /** The run of bytes a call reads, where a missing value passes as an empty one. */
    ustore_bytes_view_t bytes() const noexcept { return {pointer_, size()}; }

    bool operator==(value_view_t other) const noexcept { return std::ranges::equal(*this, other); }
};

/** Views the bytes of a contiguous container. */
template <std::ranges::contiguous_range container_type_>
    requires std::ranges::sized_range<container_type_>
value_view_t value_view(container_type_&& container) noexcept {
    return value_view_t {std::as_bytes(std::span {std::ranges::data(container), std::ranges::size(container)})};
}

/** The bytes of @p text, which a call reads without a terminator. */
inline ustore_bytes_view_t bytes_of(std::string_view text) noexcept {
    return {reinterpret_cast<ustore_byte_t const*>(text.data()), text.size()};
}

#pragma endregion Value Views

#pragma region Strided Spans

/** A pointer that advances by a byte stride instead of the size of its element.
 *  A zero stride repeats one element, so every task of a batch can share a single argument. */
template <typename element_type_>
class strided_iterator {
  public:
    using element_t = element_type_;
    using iterator_category = std::random_access_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = std::remove_cv_t<element_type_>;
    using pointer = element_type_*;
    using reference = element_type_&;

  private:
    element_type_* pointer_ {nullptr};
    ustore_size_t stride_ {0};

    element_type_* upshift(std::ptrdiff_t bytes) const noexcept { return (element_type_*)((char*)pointer_ + bytes); }
    element_type_* downshift(std::ptrdiff_t bytes) const noexcept { return (element_type_*)((char*)pointer_ - bytes); }
    std::ptrdiff_t signed_stride() const noexcept { return static_cast<std::ptrdiff_t>(stride_); }

  public:
    strided_iterator(element_type_* pointer = nullptr, ustore_size_t stride = 0) noexcept
        : pointer_(pointer), stride_(stride) {}

    element_type_& operator[](std::ptrdiff_t index) const noexcept { return *upshift(signed_stride() * index); }

    strided_iterator& operator++() noexcept {
        pointer_ = upshift(signed_stride());
        return *this;
    }

    strided_iterator& operator--() noexcept {
        pointer_ = downshift(signed_stride());
        return *this;
    }

    strided_iterator operator++(int) noexcept {
        strided_iterator const previous = *this;
        pointer_ = upshift(signed_stride());
        return previous;
    }

    strided_iterator operator--(int) noexcept {
        strided_iterator const previous = *this;
        pointer_ = downshift(signed_stride());
        return previous;
    }

    strided_iterator operator+(std::ptrdiff_t count) const noexcept {
        return {upshift(count * signed_stride()), stride_};
    }
    strided_iterator operator-(std::ptrdiff_t count) const noexcept {
        return {downshift(count * signed_stride()), stride_};
    }
    friend strided_iterator operator+(std::ptrdiff_t count, strided_iterator iterator) noexcept {
        return iterator + count;
    }

    strided_iterator& operator+=(std::ptrdiff_t count) noexcept {
        pointer_ = upshift(count * signed_stride());
        return *this;
    }

    strided_iterator& operator-=(std::ptrdiff_t count) noexcept {
        pointer_ = downshift(count * signed_stride());
        return *this;
    }

    /** The number of strides between two iterators, meaningful only for a stride that is a multiple
     *  of the element. */
    std::ptrdiff_t operator-(strided_iterator other) const noexcept {
        if (!stride_) return 0;
        return (pointer_ - other.pointer_) * static_cast<std::ptrdiff_t>(sizeof(element_type_)) / signed_stride();
    }

    explicit operator bool() const noexcept { return pointer_ != nullptr; }

    /** Whether a zero stride repeats the same element forever. */
    bool repeats() const noexcept { return !stride_; }

    /** Whether the elements sit next to each other without gaps. */
    bool is_continuous() const noexcept { return stride_ == sizeof(element_type_); }

    ustore_size_t stride() const noexcept { return stride_; }
    element_type_& operator*() const noexcept { return *pointer_; }
    element_type_* operator->() const noexcept { return pointer_; }
    element_type_* get() const noexcept { return pointer_; }

    friend bool operator==(strided_iterator const& first, strided_iterator const& second) noexcept {
        return first.pointer_ == second.pointer_;
    }

    friend std::strong_ordering operator<=>(strided_iterator const& first, strided_iterator const& second) noexcept {
        return first.pointer_ <=> second.pointer_;
    }

    /** Views one member of every element with the same stride. */
    template <typename member_type_, typename parent_type_>
    strided_iterator<std::conditional_t<std::is_const_v<element_type_>, member_type_ const, member_type_>> members(
        member_type_ parent_type_::* member_pointer) const noexcept {
        return {pointer_ ? &(pointer_->*member_pointer) : nullptr, stride_};
    }
};

/** A mutable view of individual bits packed into octets. */
class bits_span_t {
  public:

    /** A proxy for one bit that reads as and assigns from @c bool. */
    class ref_t {
        ustore_octet_t* octet_ = nullptr;
        ustore_octet_t mask_ = 0;

      public:
        ref_t(ustore_octet_t& octet) noexcept : octet_(&octet), mask_(0) {}
        ref_t(ustore_octet_t* octet, ustore_octet_t mask) noexcept : octet_(octet), mask_(mask) {}
        operator bool() const noexcept { return *octet_ & mask_; }
        ref_t& operator=(std::same_as<bool> auto value) noexcept {
            *octet_ = value ? (*octet_ | mask_) : (*octet_ & ~mask_);
            return *this;
        }
    };

    using element_t = ustore_octet_t;
    using value_type = ustore_octet_t;
    using reference = ref_t;

  private:
    element_t* begin_ {nullptr};

  public:
    bits_span_t(element_t* begin = nullptr) noexcept : begin_(begin) {}

    ref_t at(std::size_t index) const noexcept {
        return {begin_ + index / CHAR_BIT, static_cast<element_t>(1 << (index % CHAR_BIT))};
    }
    ref_t operator[](std::size_t index) const noexcept { return at(index); }

    explicit operator bool() const noexcept { return begin_ != nullptr; }
    element_t* get() const noexcept { return begin_; }
    bool operator==(bits_span_t const& other) const noexcept { return begin_ == other.begin_; }
};

/** A read-only view of individual bits packed into octets. */
class bits_view_t {
  public:
    using element_t = ustore_octet_t const;
    using value_type = bool;
    using reference = void;

  private:
    element_t* begin_ {nullptr};

  public:
    bits_view_t(element_t* begin = nullptr) noexcept : begin_(begin) {}

    bool at(std::size_t index) const noexcept {
        return begin_[index / CHAR_BIT] & static_cast<ustore_octet_t>(1 << (index % CHAR_BIT));
    }
    bool operator[](std::size_t index) const noexcept { return at(index); }

    explicit operator bool() const noexcept { return begin_ != nullptr; }
    element_t* get() const noexcept { return begin_; }
    bool operator==(bits_view_t const& other) const noexcept { return begin_ == other.begin_; }
};

/** Whether @p container_type_ lays out elements a pointer to @p element_type_ addresses, back to
 *  back. */
template <typename container_type_, typename element_type_>
concept a_contiguous_range_of =
    std::ranges::contiguous_range<container_type_> && std::ranges::sized_range<container_type_> &&
    std::same_as<std::ranges::range_value_t<container_type_>, std::remove_cv_t<element_type_>> &&
    std::convertible_to<decltype(std::ranges::data(std::declval<container_type_&>())), element_type_*>;

/** A counted run of strided elements, writable unless @p element_type_ is @c const,
 *  like @c std::span. */
template <typename element_type_>
class strided_span {
  public:
    using element_t = element_type_;
    using iterator_t = strided_iterator<element_type_>;
    using value_type = std::remove_cv_t<element_type_>;
    using reference = element_type_&;

  private:
    iterator_t begin_ {nullptr};
    ustore_size_t count_ {0};

  public:
    strided_span() = default;
    strided_span(iterator_t begin, ustore_size_t count) noexcept : begin_(begin), count_(count) {}

    /** Views a contiguous container of the same elements, stepping by their size, as @c std::span
     *  does. */
    template <typename container_type_>
        requires a_contiguous_range_of<container_type_, element_type_>
    strided_span(container_type_&& container) noexcept
        : begin_(std::ranges::data(container), sizeof(element_type_)), count_(std::ranges::size(container)) {}

    element_type_* data() const noexcept { return begin_.get(); }
    iterator_t begin() const noexcept { return begin_; }
    iterator_t end() const noexcept { return begin_ + static_cast<std::ptrdiff_t>(count_); }
    element_type_& at(std::size_t index) const noexcept { return begin_[static_cast<std::ptrdiff_t>(index)]; }
    element_type_& operator[](std::size_t index) const noexcept { return at(index); }

    strided_span<element_type_ const> immutable() const noexcept { return {{begin_.get(), begin_.stride()}, count_}; }
    strided_span subspan(std::size_t offset, std::size_t count) const noexcept {
        return {begin_ + static_cast<std::ptrdiff_t>(offset), count};
    }

    bool empty() const noexcept { return !count_; }
    std::size_t size() const noexcept { return count_; }
    ustore_size_t stride() const noexcept { return begin_.stride(); }
    ustore_size_t count() const noexcept { return count_; }
    explicit operator bool() const noexcept { return static_cast<bool>(begin_); }

    /** Views one member of every element with the same stride. */
    template <typename member_type_, typename parent_type_>
    strided_span<std::conditional_t<std::is_const_v<element_type_>, member_type_ const, member_type_>> members(
        member_type_ parent_type_::* member_pointer) const noexcept {
        return {begin_.members(member_pointer), count_};
    }

    /** Whether every element equals the first one, which a null or repeating span satisfies
     *  trivially. */
    bool same_elements() const noexcept {
        if (!begin_ || begin_.repeats()) return true;
        for (std::ptrdiff_t index = 1; index < static_cast<std::ptrdiff_t>(count_); ++index)
            if (!(begin_[index] == begin_[0])) return false;
        return true;
    }
};

/** Views a contiguous container as a strided span that steps by the size of its elements. */
template <std::ranges::contiguous_range container_type_>
    requires std::ranges::sized_range<container_type_>
strided_span<std::remove_reference_t<std::ranges::range_reference_t<container_type_>>> strided(
    container_type_&& container) noexcept {
    using element_t = std::ranges::range_value_t<container_type_>;
    return {{std::ranges::data(container), sizeof(element_t)}, std::ranges::size(container)};
}

/** Views the contiguous elements from @p begin up to @p end as a strided span. */
template <typename element_type_>
strided_span<element_type_> strided(element_type_* begin, element_type_* end) noexcept {
    return {{begin, sizeof(element_type_)}, static_cast<std::size_t>(end - begin)};
}

/** Walks any view that answers @c size and @c operator[] by position, so a view of exported
 *  outputs drives a range-based loop without a container of its own. */
template <typename container_type_>
class indexed_iterator {
    container_type_ const* container_ = nullptr;
    std::size_t index_ = 0;

  public:
    using iterator_category = std::random_access_iterator_tag;
    using difference_type = std::ptrdiff_t;
    using value_type = typename container_type_::value_type;
    using reference = value_type;
    using pointer = void;

    indexed_iterator() noexcept = default;
    indexed_iterator(container_type_ const& container, std::size_t index) noexcept
        : container_(&container), index_(index) {}

    value_type operator*() const noexcept { return (*container_)[index_]; }
    value_type operator[](difference_type offset) const noexcept { return (*container_)[index_ + offset]; }

    indexed_iterator& operator++() noexcept {
        ++index_;
        return *this;
    }
    indexed_iterator& operator--() noexcept {
        --index_;
        return *this;
    }
    indexed_iterator operator++(int) noexcept { return {*container_, index_++}; }
    indexed_iterator operator--(int) noexcept { return {*container_, index_--}; }
    indexed_iterator& operator+=(difference_type offset) noexcept {
        index_ += offset;
        return *this;
    }
    indexed_iterator operator+(difference_type offset) const noexcept { return {*container_, index_ + offset}; }
    indexed_iterator operator-(difference_type offset) const noexcept { return {*container_, index_ - offset}; }
    difference_type operator-(indexed_iterator const& other) const noexcept {
        return static_cast<difference_type>(index_) - static_cast<difference_type>(other.index_);
    }

    bool operator==(indexed_iterator const& other) const noexcept { return index_ == other.index_; }
    std::strong_ordering operator<=>(indexed_iterator const& other) const noexcept { return index_ <=> other.index_; }
};

#pragma endregion Strided Spans

#pragma region Tapes

/** Strings a call exported into its arena as a tape of either offset width, read in place. */
class strings_view_t {
    ustore_strings_view_t tape_ {};
    std::size_t count_ = 0;

    /** Where string @p index starts, read at the width the layout names. */
    std::size_t offset_(std::size_t index) const noexcept {
        if (tape_.layout == ustore_strings_u64_tape_k)
            return static_cast<std::size_t>(tape_.data.u64_tape.offsets[index]);
        return tape_.data.u32_tape.offsets[index];
    }

    char const* data_() const noexcept {
        if (tape_.layout == ustore_strings_u64_tape_k) return reinterpret_cast<char const*>(tape_.data.u64_tape.data);
        return reinterpret_cast<char const*>(tape_.data.u32_tape.data);
    }

  public:
    using value_type = std::string_view;

    strings_view_t() noexcept = default;
    strings_view_t(ustore_strings_view_t tape, std::size_t count) noexcept : tape_(tape), count_(count) {}

    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return !count_; }

    /** The string at @p index, below @c size, whose offsets never decrease, as every call writes
     *  them. */
    std::string_view operator[](std::size_t index) const noexcept {
        std::size_t const begin = offset_(index), end = offset_(index + 1);
        return {data_() + begin, end - begin};
    }

    indexed_iterator<strings_view_t> begin() const noexcept { return {*this, 0}; }
    indexed_iterator<strings_view_t> end() const noexcept { return {*this, count_}; }

    /** The tape as the call wrote it, which another call takes back as it is. */
    ustore_strings_view_t tape() const noexcept { return tape_; }
};

/** The strings of an output @p tape from @p first on, viewing the same bytes, as a slice keeps its
 *  parent's offsets. */
inline ustore_strings_view_t tape_slice(ustore_strings_view_t tape, std::size_t first) noexcept {
    if (tape.layout == ustore_strings_u64_tape_k) {
        if (tape.data.u64_tape.offsets) tape.data.u64_tape.offsets += first;
    }
    else if (tape.data.u32_tape.offsets) tape.data.u32_tape.offsets += first;
    return tape;
}

/** A tape of 64-bit offsets, where string @c i spans @b [offsets[i],offsets[i+1]) of @p data. */
inline ustore_strings_view_t u64_tape(void const* data, std::uint64_t const* offsets) noexcept {
    ustore_strings_view_t tape {.layout = ustore_strings_u64_tape_k};
    tape.data.u64_tape = {static_cast<ustore_byte_t const*>(data), offsets};
    return tape;
}

#pragma endregion Tapes

#pragma region Argument Views

using keys_view_t = strided_span<ustore_key_t const>;

/** One task of a batched read or write. */
struct place_t {
    ustore_collection_id_t collection;
    ustore_key_t const& key;
    std::string_view field;

    collection_key_t collection_key() const noexcept { return {collection, key}; }
};

/** The locations of a batched read or write, as a call receives them. */
struct place_batch_t {
    using value_type = place_t;
    strided_iterator<ustore_collection_id_t const> collections_begin;
    strided_iterator<ustore_key_t const> keys_begin;
    strided_iterator<std::string_view const> fields_begin;
    ustore_size_t count {0};

    std::size_t size() const noexcept { return count; }
    place_t operator[](std::size_t index) const noexcept {
        std::ptrdiff_t const signed_index = static_cast<std::ptrdiff_t>(index);
        ustore_collection_id_t collection = collections_begin ? collections_begin[signed_index]
                                                              : ustore_collection_id_t {};
        ustore_key_t const& key = keys_begin[signed_index];
        std::string_view field = fields_begin ? fields_begin[signed_index] : std::string_view {};
        return {collection, key, field};
    }

    /** Whether every task addresses the same collection. */
    bool same_collection() const noexcept {
        return strided_span<ustore_collection_id_t const>(collections_begin, count).same_elements();
    }

    /** The collections as a call takes them. */
    ustore_collections_view_t collections() const noexcept {
        return {collections_begin.get(), collections_begin.stride()};
    }

    /** The keys as a call takes them. */
    ustore_keys_view_t keys() const noexcept { return {keys_begin.get(), keys_begin.stride()}; }
};

/** Edges as three strided columns of sources, targets and edge identifiers, which the offsets of
 *  the read that exported them split by vertex. */
template <typename id_type_>
struct edges_span {

    using id_t = id_type_;
    using tuple_t = std::conditional_t<std::is_const_v<id_type_>, edge_t const, edge_t>;

    strided_span<id_type_> sources;
    strided_span<id_type_> targets;
    strided_span<id_type_> ids;

    /** One offset per vertex of the read plus one, counted in edges, where a missing vertex spans
     *  none; empty for edges no read exported. */
    std::span<ustore_length_t const> offsets;

    edges_span() = default;

    edges_span(strided_span<id_type_> sources, strided_span<id_type_> targets, strided_span<id_type_> ids) noexcept
        : sources(sources), targets(targets), ids(ids) {}

    /** Edges between @p sources and @p targets that all hold the default edge identifier. */
    edges_span(strided_span<id_type_> sources, strided_span<id_type_> targets) noexcept
        : sources(sources), targets(targets) {
        static_assert(std::is_const_v<id_type_>,
                      "Only a read-only view can point at the shared default edge identifier");
        ids = {{&ustore_edge_id_default_k, 0}, 1};
    }

    edges_span(tuple_t* begin, tuple_t* end, std::span<ustore_length_t const> offsets = {}) noexcept
        : offsets(offsets) {
        strided_span<tuple_t> const tuples({begin, sizeof(tuple_t)}, static_cast<std::size_t>(end - begin));
        sources = tuples.members(&edge_t::source);
        targets = tuples.members(&edge_t::target);
        ids = tuples.members(&edge_t::id);
    }

    /** Views a contiguous container of edges as strided edge columns. */
    template <typename tuples_type_>
        requires a_contiguous_range_of<tuples_type_, tuple_t>
    edges_span(tuples_type_&& tuples) noexcept
        : edges_span(std::ranges::data(tuples), std::ranges::data(tuples) + std::ranges::size(tuples)) {}

    std::size_t size() const noexcept { return sources.size() < targets.size() ? sources.size() : targets.size(); }

    edge_t operator[](std::size_t index) const noexcept { return {sources[index], targets[index], ids[index]}; }

    /** The edges of the vertex at @p index of the read that exported them. */
    edges_span of_vertex(std::size_t index) const noexcept {
        std::size_t const begin = offsets[index], count = offsets[index + 1] - begin;
        return {sources.subspan(begin, count), targets.subspan(begin, count), ids.subspan(begin, count)};
    }

    operator edges_span<id_type_ const>() const noexcept { return immutable(); }
    edges_span<id_type_ const> immutable() const noexcept {
        edges_span<id_type_ const> result {sources.immutable(), targets.immutable(), ids.immutable()};
        result.offsets = offsets;
        return result;
    }
};

using edges_view_t = edges_span<ustore_key_t const>;

/** The edges a read of @p count vertices exported into its arena, split by vertex through its @p
 *  offsets. */
inline edges_view_t exported_edges(ustore_key_t const* edges, ustore_length_t const* offsets,
                                   std::size_t count) noexcept {
    if (!offsets) return {};
    edge_t const* const begin = reinterpret_cast<edge_t const*>(edges);
    return {begin, begin + offsets[count], {offsets, count + 1}};
}

/** Views a contiguous container of @c edge_t as strided edge columns, writable unless it is
 *  @c const. */
template <std::ranges::contiguous_range tuples_type_>
    requires std::ranges::sized_range<tuples_type_>
std::conditional_t<std::is_const_v<std::remove_reference_t<std::ranges::range_reference_t<tuples_type_>>>, edges_view_t,
                   edges_span<ustore_key_t>>
edges(tuples_type_&& tuples) noexcept {
    return {std::ranges::data(tuples), std::ranges::data(tuples) + std::ranges::size(tuples)};
}

#pragma endregion Argument Views

#pragma region Locations

/** A key inside a collection, optionally narrowed down to one field of its document. */
struct collection_key_field_t {

    /** Collection owning the key. */
    ustore_collection_id_t collection {};

    /** Key inside the collection. */
    ustore_key_t key {ustore_key_unknown_k};

    /** Path of the document field, or an empty one for the whole value. */
    std::string_view field {};

    collection_key_field_t() = default;
    collection_key_field_t(ustore_collection_id_t collection, ustore_key_t key, std::string_view field = {}) noexcept
        : collection(collection), key(key), field(field) {}

    /** A field of @p key in whichever collection the handle addressing it names. */
    explicit collection_key_field_t(ustore_key_t key, std::string_view field) noexcept : key(key), field(field) {}
};

/** A location that names the collection it lives in. */
template <typename location_type_>
concept a_collection_location = requires(location_type_ const& location) { location.collection; };

/** A location that narrows its value down to one document field. */
template <typename location_type_>
concept a_field_location = requires(location_type_ const& location) { location.field; };

/** A single key with its collection, that widens into a @c collection_key_field_t. */
template <typename location_type_>
concept a_key_location = std::same_as<std::remove_cvref_t<location_type_>, collection_key_t>;

/** A batch of locations or values rather than a single one, recognized by its
 *  nested @c value_type. */
template <typename batch_type_>
concept a_batch = requires { typename std::remove_cvref_t<batch_type_>::value_type; };

/** A batch that holds its own byte stride instead of stepping by the size of its elements. */
template <typename batch_type_>
concept a_strided_batch = a_batch<batch_type_> &&
                          requires(std::remove_cvref_t<batch_type_> const& elements) { elements.stride(); };

/** Resolves a single location or value to itself. */
template <typename batch_type_>
struct batch_element {
    using type = std::remove_cvref_t<batch_type_>;
};

/** Resolves a batch to the type of its elements. */
template <a_batch batch_type_>
struct batch_element<batch_type_> {
    using type = typename std::remove_cvref_t<batch_type_>::value_type;
};

template <typename batch_type_>
using batch_element_of = typename batch_element<batch_type_>::type;

/** Stores a location by value and hands out references to it. */
template <typename location_type_>
class location_store {
  public:
    using plain_t = location_type_;
    using store_t = location_type_;

  private:
    store_t store_ {};

  public:
    location_store() = default;
    location_store(plain_t location) noexcept : store_(std::move(location)) {}

    plain_t& ref() noexcept { return store_; }
    plain_t const& ref() const noexcept { return store_; }
};

/** Widens a single key into a full location stored by value. */
template <a_key_location location_type_>
class location_store<location_type_> {
  public:
    using plain_t = collection_key_field_t;
    using store_t = collection_key_field_t;

  private:
    store_t store_ {};

  public:
    location_store() = default;
    location_store(plain_t location) noexcept : store_(std::move(location)) {}

    plain_t& ref() noexcept { return store_; }
    plain_t const& ref() const noexcept { return store_; }
};

/** Stores a caller-owned location by pointer, so a batch is never copied. */
template <typename location_type_>
    requires(!a_key_location<location_type_>)
class location_store<location_type_&> {
  public:
    using plain_t = location_type_;
    using store_t = location_type_*;

  private:
    store_t store_ {nullptr};

  public:
    location_store() = default;
    location_store(plain_t& location) noexcept : store_(&location) {}

    plain_t& ref() noexcept { return *store_; }
    plain_t const& ref() const noexcept { return *store_; }
};

/** A batch of locations that all live in one collection the batch does not name itself. */
template <typename locations_type_>
struct locations_in_collection {

    /** The locations, stored by value or by reference. */
    location_store<locations_type_> without;

    /** Collection every location lives in. */
    ustore_collection_id_t collection;
};

/** Resolves a location or a batch of locations into the strided keys, collections and fields a
 *  call takes. */
template <typename locations_type_>
struct places_arg_extractor {

    using location_t = locations_type_;
    using element_t = batch_element_of<locations_type_>;
    static constexpr bool is_one_k = !a_batch<locations_type_>;

    ustore_size_t count(location_t const& locations) noexcept {
        if constexpr (is_one_k) return 1;
        else return std::size(locations);
    }

    strided_iterator<ustore_key_t const> keys(location_t const& locations) noexcept {
        if constexpr (std::same_as<element_t, ustore_key_t>) return elements(locations);
        else return elements(locations).members(&element_t::key);
    }

    strided_iterator<ustore_collection_id_t const> collections(location_t const& locations) noexcept {
        if constexpr (a_collection_location<element_t>) return elements(locations).members(&element_t::collection);
        else return {};
    }

    strided_iterator<std::string_view const> fields(location_t const& locations) noexcept {
        if constexpr (a_field_location<element_t>) return elements(locations).members(&element_t::field);
        else return {};
    }

  private:
    /** A single location repeats itself. */
    static strided_iterator<element_t const> elements(location_t const& locations) noexcept
        requires is_one_k
    {
        return {&locations, 0};
    }

    /** A batch that holds its stride steps by it. */
    static strided_iterator<element_t const> elements(location_t const& locations) noexcept
        requires(!is_one_k && a_strided_batch<location_t>)
    {
        return {std::data(locations), locations.stride()};
    }

    /** Any other batch steps by the size of its elements. */
    static strided_iterator<element_t const> elements(location_t const& locations) noexcept
        requires(!is_one_k && !a_strided_batch<location_t>)
    {
        return {std::data(locations), sizeof(element_t)};
    }
};

/** Passes the locations a call takes through unchanged. */
template <>
struct places_arg_extractor<place_batch_t> {

    using location_t = place_batch_t;
    static constexpr bool is_one_k = false;

    ustore_size_t count(place_batch_t const& places) noexcept { return places.count; }
    strided_iterator<ustore_key_t const> keys(place_batch_t const& places) noexcept { return places.keys_begin; }
    strided_iterator<ustore_collection_id_t const> collections(place_batch_t const& places) noexcept {
        return places.collections_begin;
    }
    strided_iterator<std::string_view const> fields(place_batch_t const& places) noexcept {
        return places.fields_begin;
    }
};

/** Resolves a batch through its own extractor and repeats the one collection it lives in. */
template <typename locations_type_>
struct places_arg_extractor<locations_in_collection<locations_type_>> {

    using location_t = locations_in_collection<locations_type_>;
    using batch_extractor_t = places_arg_extractor<std::remove_reference_t<locations_type_>>;
    static constexpr bool is_one_k = batch_extractor_t::is_one_k;

    ustore_size_t count(location_t const& locations) noexcept {
        return batch_extractor_t {}.count(locations.without.ref());
    }
    strided_iterator<ustore_key_t const> keys(location_t const& locations) noexcept {
        return batch_extractor_t {}.keys(locations.without.ref());
    }
    strided_iterator<ustore_collection_id_t const> collections(location_t const& locations) noexcept {
        return {&locations.collection};
    }
    strided_iterator<std::string_view const> fields(location_t const& locations) noexcept {
        return batch_extractor_t {}.fields(locations.without.ref());
    }
};

/** The places a call takes that @p locations resolve into, viewing them where they lie. */
template <typename locations_type_>
place_batch_t locations_places_(locations_type_ const& locations) noexcept {
    places_arg_extractor<locations_type_> extractor;
    return {.collections_begin = extractor.collections(locations),
            .keys_begin = extractor.keys(locations),
            .fields_begin = extractor.fields(locations),
            .count = extractor.count(locations)};
}

/** The places of @p keys inside the one @p collection, which outlives every call given them. */
inline place_batch_t places_in(ustore_collection_id_t const& collection, keys_view_t keys) noexcept {
    return {.collections_begin = {&collection, 0}, .keys_begin = keys.begin(), .count = keys.size()};
}

/** Where @p key lives inside @p collection. */
inline collection_key_field_t location_in(ustore_collection_id_t collection, ustore_key_t key) noexcept {
    return {collection, key};
}

/** Where @p located lives once moved into @p collection, which wins over its own, keeping the field
 *  it names. */
template <a_field_location location_type_>
collection_key_field_t location_in(ustore_collection_id_t collection, location_type_ const& located) noexcept {
    return {collection, located.key, located.field};
}

/** Where @p located lives once moved into @p collection, which wins over its own. */
template <typename location_type_>
collection_key_field_t location_in(ustore_collection_id_t collection, location_type_ const& located) noexcept {
    return {collection, located.key};
}

#pragma endregion Locations

#pragma region String Batches

/** An element a batch of strings may hold: a value view, a null-terminated string, or anything that
 *  converts into a string view. */
template <typename element_type_>
concept a_string_like_element = std::same_as<element_type_, value_view_t> ||
                                std::is_convertible_v<element_type_ const&, std::string_view>;

/** Whether @p text_type_ is a null-terminated string, which may be null. */
template <typename text_type_>
concept a_c_string = std::is_convertible_v<text_type_ const&, char const*>;

/** The bytes @p text spells as an element of a batch, where a missing value spells an empty one. */
inline ustore_bytes_view_t bytes_of(value_view_t const& text) noexcept { return text.bytes(); }

/** The bytes @p text spells as an element of a batch, where a null string spells an empty one. */
template <a_string_like_element text_type_>
    requires a_c_string<text_type_>
ustore_bytes_view_t bytes_of(text_type_ const& text) noexcept {
    char const* const string = text;
    return string ? bytes_of(std::string_view {string}) : ustore_bytes_view_t {};
}

/** The bytes @p text spells as an element of a batch, through the string view it converts into. */
template <a_string_like_element text_type_>
ustore_bytes_view_t bytes_of(text_type_ const& text) noexcept {
    return bytes_of(std::string_view {text});
}

/**
 *  @brief Passes a strided run of string-like elements to a call through an accessor, where a
 *      zero stride repeats the first element for every task.
 *
 *  The view it hands out points at the object itself, so the object outlives every call
 *  given the view.
 */
template <a_string_like_element element_type_>
class strided_strings {
    strided_iterator<element_type_ const> elements_ {};

    static void get_(void const* handle, ustore_size_t const* indices, ustore_size_t count,
                     ustore_bytes_view_t* views) noexcept {
        strided_iterator<element_type_ const> const elements = static_cast<strided_strings const*>(handle)->elements_;
        for (ustore_size_t index = 0; index != count; ++index)
            views[index] = bytes_of(elements[static_cast<std::ptrdiff_t>(indices[index])]);
    }

  public:
    strided_strings() noexcept = default;
    explicit strided_strings(strided_iterator<element_type_ const> elements) noexcept : elements_(elements) {}

    /** The accessor over the run, or a zeroed view, the input's default, for a null run. */
    ustore_strings_view_t view() const noexcept {
        ustore_strings_view_t view {};
        if (!elements_) return view;
        view.layout = ustore_strings_accessor_k;
        view.data.accessor = {&get_, this};
        return view;
    }
};

/** Answers the string at every index of a contiguous run of @p element_type_ that
 *  @p handle starts. */
template <a_string_like_element element_type_>
void contiguous_strings_get_(void const* handle, ustore_size_t const* indices, ustore_size_t count,
                             ustore_bytes_view_t* views) noexcept {
    element_type_ const* const elements = static_cast<element_type_ const*>(handle);
    for (ustore_size_t index = 0; index != count; ++index) views[index] = bytes_of(elements[indices[index]]);
}

/** Answers the first string of a contiguous run of @p element_type_ that @p handle starts, for
 *  every index. */
template <a_string_like_element element_type_>
void repeated_strings_get_(void const* handle, ustore_size_t const*, ustore_size_t count,
                           ustore_bytes_view_t* views) noexcept {
    element_type_ const& element = *static_cast<element_type_ const*>(handle);
    for (ustore_size_t index = 0; index != count; ++index) views[index] = bytes_of(element);
}

/** Answers the first string of the exported view @p handle points to, for every index. */
template <typename view_type_>
void repeated_view_get_(void const* handle, ustore_size_t const*, ustore_size_t count,
                        ustore_bytes_view_t* views) noexcept {
    ustore_bytes_view_t const first = bytes_of((*static_cast<view_type_ const*>(handle))[0]);
    for (ustore_size_t index = 0; index != count; ++index) views[index] = first;
}

/** Whether @p strings_type_ lays out string-like elements back to back. */
template <typename strings_type_>
concept a_contiguous_string_range = std::ranges::contiguous_range<strings_type_> &&
                                    std::ranges::sized_range<strings_type_> &&
                                    a_string_like_element<std::ranges::range_value_t<strings_type_>>;

/** The tape a batch of strings already is, read where it lies. */
inline ustore_strings_view_t strings_view_of(strings_view_t const& strings) noexcept { return strings.tape(); }

/** An accessor whose handle is the first element of @p strings itself, so the view needs no object
 *  of its own and stays valid while @p strings does. */
template <a_contiguous_string_range strings_type_>
ustore_strings_view_t strings_view_of(strings_type_ const& strings) noexcept {
    using element_t = std::ranges::range_value_t<strings_type_>;
    ustore_strings_view_t view {};
    if (std::ranges::empty(strings)) return view;
    view.layout = ustore_strings_accessor_k;
    view.data.accessor = {&contiguous_strings_get_<element_t>, std::ranges::data(strings)};
    return view;
}

/** The first string of a tape, repeated for every task, through an accessor whose handle is the
 *  tape. */
inline ustore_strings_view_t repeated_view_of(strings_view_t const& strings) noexcept {
    ustore_strings_view_t view {.layout = ustore_strings_accessor_k};
    view.data.accessor = {&repeated_view_get_<strings_view_t>, &strings};
    return view;
}

/** The first string of a contiguous batch, repeated for every task, through an accessor whose
 *  handle is it. */
template <a_contiguous_string_range strings_type_>
ustore_strings_view_t repeated_view_of(strings_type_ const& strings) noexcept {
    using element_t = std::ranges::range_value_t<strings_type_>;
    ustore_strings_view_t view {.layout = ustore_strings_accessor_k};
    view.data.accessor = {&repeated_strings_get_<element_t>, std::ranges::data(strings)};
    return view;
}

/** A batch of strings a verb reads where it lies, so a deferred call views it: a tape, or values
 *  laid out back to back. */
template <typename strings_type_>
concept a_strings_batch = requires(strings_type_ const& strings) {
    { strings_view_of(strings) } -> std::same_as<ustore_strings_view_t>;
    { std::size(strings) } -> std::convertible_to<std::size_t>;
};

#pragma endregion String Batches

#pragma region Read Results

/** The lengths a read exports into an arena beside its presence bits, where an absent value reads
 *  as @c missing_length_k, and a read that left its presences out reports every value present. */
class lengths_view_t {
    ustore_size_t count_ {0};
    ustore_octet_t const* presences_ {nullptr};
    ustore_length_t const* lengths_ {nullptr};

  public:
    using value_type = ustore_length_t;
    using iterator_t = indexed_iterator<lengths_view_t>;

    lengths_view_t() = default;
    lengths_view_t(ustore_size_t count, ustore_octet_t const* presences, ustore_length_t const* lengths) noexcept
        : count_(count), presences_(presences), lengths_(lengths) {}

    std::size_t size() const noexcept { return count_; }
    bool empty() const noexcept { return !count_; }

    ustore_length_t operator[](std::size_t index) const noexcept {
        if (presences_ && !bits_view_t {presences_}[index]) return missing_length_k;
        return lengths_[index];
    }

    iterator_t begin() const noexcept { return {*this, 0}; }
    iterator_t end() const noexcept { return {*this, size()}; }

    bits_view_t presences() const noexcept { return {presences_}; }
    ustore_length_t const* lengths() const noexcept { return lengths_; }
};

/** The values a read exports into an arena as Apache Arrow does: presence bits and the tape of
 *  values, of either offset width, where an absent value reads as a missing view. */
class values_view_t {
    ustore_octet_t const* presences_ {nullptr};
    strings_view_t strings_ {};

  public:
    using value_type = value_view_t;
    using iterator_t = indexed_iterator<values_view_t>;

    values_view_t() = default;
    values_view_t(ustore_size_t count, ustore_octet_t const* presences, ustore_strings_view_t tape) noexcept
        : presences_(presences), strings_(tape, count) {}

    std::size_t size() const noexcept { return strings_.size(); }
    bool empty() const noexcept { return strings_.empty(); }

    value_view_t operator[](std::size_t index) const noexcept {
        if (presences_ && !bits_view_t {presences_}[index]) return {};
        return strings_[index];
    }

    iterator_t begin() const noexcept { return {*this, 0}; }
    iterator_t end() const noexcept { return {*this, size()}; }

    bits_view_t presences() const noexcept { return {presences_}; }

    /** The tape the values live in, where a missing value is an empty one, which a write takes back
     *  as it is. */
    ustore_strings_view_t tape() const noexcept { return strings_.tape(); }
};

/** The tape a read exported, written back as it arrived. */
inline ustore_strings_view_t strings_view_of(values_view_t const& strings) noexcept { return strings.tape(); }

/** The first value a read exported, repeated for every task, through an accessor whose handle is
 *  the values. */
inline ustore_strings_view_t repeated_view_of(values_view_t const& values) noexcept {
    ustore_strings_view_t view {.layout = ustore_strings_accessor_k};
    view.data.accessor = {&repeated_view_get_<values_view_t>, &values};
    return view;
}

/** Where a read writes the presence bits it answers. */
template <>
struct outputs<bits_view_t> {

    /** Keys read, which the bits span. */
    ustore_size_t count = 0;

    /** Output for one bit per key, inside the arena. */
    ustore_octet_t const* presences = nullptr;

    bits_view_t answer() const noexcept { return bits_view_t {presences}; }
};

/** Where a read writes the lengths it answers beside their presence bits. */
template <>
struct outputs<lengths_view_t> {

    /** Keys read, the length of every output. */
    ustore_size_t count = 0;

    /** Output for one bit per key, inside the arena. */
    ustore_octet_t const* presences = nullptr;

    /** Output for one length per key, inside the arena. */
    ustore_length_t const* lengths = nullptr;

    lengths_view_t answer() const noexcept { return {count, presences, lengths}; }
};

/** Where a read writes the values it answers beside their presence bits. */
template <>
struct outputs<values_view_t> {

    /** Keys read, the length of every output. */
    ustore_size_t count = 0;

    /** Output for one bit per key, inside the arena. */
    ustore_octet_t const* presences = nullptr;

    /** Output for the tape of values, inside the arena. */
    ustore_strings_view_t values {};

    values_view_t answer() const noexcept { return {count, presences, values}; }
};

/** The answer for a batch as @p answer_type_ asks, which is either the batch or its only element,
 *  as a proxy over one location answers. */
template <typename answer_type_, typename batch_type_>
expected<answer_type_> first_or_all_(expected<batch_type_> found) noexcept {
    if constexpr (std::same_as<answer_type_, batch_type_>) return found;
    else if (found.status.failed()) return {{}, found.status};
    else return {found.value[0], found.status};
}

/** Where a read of a batch asks for lengths: nowhere, unless it answers them. */
template <typename answer_type_>
ustore_length_t const** lengths_output_(outputs<answer_type_>&) noexcept {
    return nullptr;
}

inline ustore_length_t const** lengths_output_(outputs<lengths_view_t>& written) noexcept { return &written.lengths; }

/** Where a read of a batch asks for values: nowhere, unless it answers them. */
template <typename answer_type_>
ustore_strings_view_t* values_output_(outputs<answer_type_>&) noexcept {
    return nullptr;
}

inline ustore_strings_view_t* values_output_(outputs<values_view_t>& written) noexcept { return &written.values; }

/** Where a read writes the key of every string it was asked for. */
template <>
struct outputs<keys_view_t> {

    /** Strings read, the length of the output. */
    ustore_size_t count = 0;

    /** Output for one key per string, inside the arena. */
    ustore_key_t const* keys = nullptr;

    keys_view_t answer() const noexcept { return {{keys, sizeof(ustore_key_t)}, keys ? count : 0}; }
};

/** Where a call writes a tape of strings and how many it holds. */
template <>
struct outputs<strings_view_t> {

    /** Output for the number of strings. */
    ustore_size_t written = 0;

    /** Output for the tape of strings, inside the arena. */
    ustore_strings_view_t strings {};

    strings_view_t answer() const noexcept { return {strings, written}; }
};

#pragma endregion Read Results

#pragma region Value Adapters

/** Resolves values into the strings a call takes, specialized per decayed value type, keeping what
 *  an accessor reads for as long as the extractor lives. */
template <typename values_type_>
struct values_arg_extractor {};

/** Many null-terminated strings. */
template <>
struct values_arg_extractor<char const**> {
    strided_strings<char const*> strings;

    ustore_strings_view_t view(char const** values) noexcept {
        strings = strided_strings<char const*> {{values, sizeof(char const*)}};
        return strings.view();
    }
};

/** One null-terminated string, repeated for every key. */
template <>
struct values_arg_extractor<char const*> {
    char const* repeated = nullptr;
    strided_strings<char const*> strings;

    ustore_strings_view_t view(char const* value) noexcept {
        repeated = value;
        strings = strided_strings<char const*> {{&repeated, 0}};
        return strings.view();
    }
};

/** Many value views laid out one after another, as an array decays into. */
template <typename view_type_>
    requires std::same_as<std::remove_const_t<view_type_>, value_view_t>
struct values_arg_extractor<view_type_*> {
    strided_strings<value_view_t> strings;

    ustore_strings_view_t view(view_type_* values) noexcept {
        strings = strided_strings<value_view_t> {{values, sizeof(value_view_t)}};
        return strings.view();
    }
};

/** One value view, repeated for every key. */
template <>
struct values_arg_extractor<value_view_t> {
    strided_strings<value_view_t> strings;

    ustore_strings_view_t view(value_view_t const& value) noexcept {
        strings = strided_strings<value_view_t> {{&value, 0}};
        return strings.view();
    }
};

/** Strings a call reads as they are, a tape or an accessor. */
template <>
struct values_arg_extractor<ustore_strings_view_t> {
    ustore_strings_view_t view(ustore_strings_view_t const& values) noexcept { return values; }
};

/** Values a read exported, written back as the tape they arrived in. */
template <>
struct values_arg_extractor<values_view_t> {
    ustore_strings_view_t view(values_view_t const& values) noexcept { return values.tape(); }
};

/** Strings a call exported, written back as the tape they arrived in. */
template <>
struct values_arg_extractor<strings_view_t> {
    ustore_strings_view_t view(strings_view_t const& values) noexcept { return values.tape(); }
};

/** A contiguous container of value views or of strings, read through an accessor. */
template <std::ranges::contiguous_range strings_type_>
    requires std::ranges::sized_range<strings_type_> && a_string_like_element<std::ranges::range_value_t<strings_type_>>
struct values_arg_extractor<strings_type_> {
    using element_t = std::ranges::range_value_t<strings_type_>;
    strided_strings<element_t> strings;

    ustore_strings_view_t view(strings_type_ const& values) noexcept {
        if (std::ranges::empty(values)) return {};
        strings = strided_strings<element_t> {{std::ranges::data(values), sizeof(element_t)}};
        return strings.view();
    }
};

#pragma endregion Value Adapters

#pragma region Executors and Arenas

/** Owns one executor, the threads or I/O rings that run calls on any number of databases, one at a
 *  time; an empty one runs every call on the caller's own thread. */
class executor_t {
    owner<ustore_executor_t, ustore_executor_free> owner_;

  public:
    ustore_executor_t handle() const noexcept { return owner_.handle(); }

    /** The output field a constructor writes the new executor into. */
    ustore_executor_t* handle_field() noexcept { return owner_.handle_field(); }

    /** Creates an executor of host threads for the RAM, RocksDB and LevelDB engines. */
    static expected<executor_t> for_ram(ustore_executor_for_ram_t arguments = {}) noexcept {
        return opened_<executor_t>(ustore_executor_for_ram, arguments, &ustore_executor_for_ram_t::executor);
    }

    /** Creates an executor for the NVMe engine, whose every thread submits and polls its own I/O
     *  ring, which tells the databases opened on it how to move bytes. */
    static expected<executor_t> for_nvme(ustore_executor_for_nvme_t arguments = {}) noexcept {
        return opened_<executor_t>(ustore_executor_for_nvme, arguments, &ustore_executor_for_nvme_t::executor);
    }

    /** Orders work on one stream on every device @p arguments name by their UStore ordinals, or on
     *  every device of the first vendor with a known class for none, each opened or adopted from
     *  the caller and destroyed with the executor, all fed by the calling thread. An NVMe database
     *  compacts on its first device. */
    static expected<executor_t> for_vram(ustore_executor_for_vram_t arguments = {}) noexcept {
        return opened_<executor_t>(ustore_executor_for_vram, arguments, &ustore_executor_for_vram_t::executor);
    }

    /** Waits until no call holds the executor, then parks or spins its threads as @p mood asks. */
    status_t drain(ustore_mood_t mood = ustore_mood_parked_k) noexcept {
        return answered_<status_t>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
            ustore_executor_drain_t arguments {.executor = handle(), .outcome = outcome, .mood = mood};
            ustore_executor_drain(&arguments);
        });
    }
};

/** Creates an @p owner_type_ through @p verb, blocking on @p executor, or on the calling thread
 *  without one, as every open and load does. */
template <typename owner_type_, typename arguments_type_, typename handle_type_>
expected<owner_type_> opened_(void (*verb)(arguments_type_*), arguments_type_ arguments,
                              handle_type_* arguments_type_::* field, executor_t const& executor) noexcept {
    arguments.executor = executor.handle();
    arguments.completion = ustore_completion_blocking_k;
    return opened_<owner_type_>(verb, arguments, field);
}

/** Frees @p arena as its owner dies, which an open export of its outputs refuses, leaving it to the
 *  export's holder, who closes every export before the owner dies. */
inline void free_arena(ustore_arena_t arena) noexcept {
    [[maybe_unused]] ustore_status_t const freed = ustore_arena_free(arena);
}

/** Owns one arena made where an executor computes, which calls fill with results and which is
 *  freed before the executor that made it; every call writing outputs into it rewinds it first. */
class arena_t {
    owner<ustore_arena_t, free_arena> owner_;

  public:
    ustore_arena_t handle() const noexcept { return owner_.handle(); }

    /** The output field @c make writes the new arena into. */
    ustore_arena_t* handle_field() noexcept { return owner_.handle_field(); }

    /** Makes an arena where the executor @p arguments names computes, or on host pages for calls on
     *  the calling thread. */
    static expected<arena_t> make(ustore_arena_init_t arguments = {}) noexcept {
        return opened_<arena_t>(ustore_arena_init, arguments, &ustore_arena_init_t::arena);
    }

    /** Where the outputs live, as the library reports it. */
    expected<ustore_arena_description_t> describe() const noexcept {
        return answered_<expected<ustore_arena_description_t>>(
            [&](ustore_outcome_t* outcome, outputs<ustore_arena_description_t>* written) noexcept {
                ustore_arena_describe_t arguments {.arena = handle(),
                                                   .outcome = outcome,
                                                   .description = &written->value};
                ustore_arena_describe(&arguments);
            });
    }
};

/**
 *  @brief Lists the GPUs this process can drive into @p devices, NVIDIA first, then AMD, then
 *      Apple, so the position of a device is the ordinal @c executor_t::for_vram names it by,
 *      answering how many there are, which may exceed what @p devices holds.
 *
 *  A machine without GPUs lists none and answers zero. @c ustore_gpu_vendor_name and
 *  @c ustore_gpu_class_name spell the vendor and the class of every device.
 */
inline expected<std::size_t> gpu_devices(std::span<ustore_gpu_device_t> devices) noexcept {
    ustore_size_t count = 0;
    status_t const status = answered_<status_t>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_gpu_devices_t arguments {.outcome = outcome,
                                        .devices = devices.data(),
                                        .devices_capacity = devices.size(),
                                        .devices_count = &count};
        ustore_gpu_devices(&arguments);
    });
    return {static_cast<std::size_t>(count), status};
}

/** The linked library's version, a release-candidate suffix included. */
inline std::string_view version_name() noexcept { return ustore_version_name(); }

#pragma endregion Executors and Arenas

#pragma region Receivers

/** The stamp a transaction or a snapshot reads at, which a commit validates against. */
enum class read_stamp_t : ustore_stamp_t {};

/** A commit's position in commit order, visible to every snapshot reading at or above it. */
enum class commit_stamp_t : ustore_stamp_t {};

/** Where a stage writes the stamp the transaction reads at. */
template <>
struct outputs<read_stamp_t> {

    /** The stamp, as a call writes it. */
    ustore_stamp_t stamp = 0;

    read_stamp_t answer() const noexcept { return read_stamp_t {stamp}; }
};

/** Where a commit writes its position in commit order. */
template <>
struct outputs<commit_stamp_t> {

    /** The stamp, as a call writes it. */
    ustore_stamp_t stamp = 0;

    commit_stamp_t answer() const noexcept { return commit_stamp_t {stamp}; }
};

/** What a collection handle may do with the collection it names: a snapshot's handles only read. */
enum class collection_access_t : bool {
    read_only_k = false,
    read_write_k = true,
};

/** What every call made through one receiver names before its inputs: the database, the executor
 *  that runs it, and the transaction or snapshot its reads see and its writes go through. */
struct receiver_t {

    /** An open database. */
    ustore_database_t database = nullptr;

    /** Executor that runs the calls, where @c nullptr runs them on the calling thread. */
    ustore_executor_t executor = nullptr;

    /** Transaction the writes go through and the reads see, or @c nullptr for the head. */
    ustore_transaction_t transaction = nullptr;

    /** Snapshot the reads see when no transaction is named, or @c nullptr for the head. */
    ustore_snapshot_t snapshot = nullptr;

    /** The view a read sees, which inside a transaction records the keys it reads unless @p watch
     *  says otherwise. */
    ustore_view_t view(ustore_watch_t watch) const noexcept {
        return transaction ? ustore_transaction_view(transaction, watch) : ustore_snapshot_view(snapshot);
    }

    /** The same receiver, running its calls on @p runner. */
    receiver_t on(ustore_executor_t runner) const noexcept { return {database, runner, transaction, snapshot}; }
};

/** Drops what @p mode names of @p collection: its values, its pairs, or the collection with its
 *  name, the one body every collection handle and every database receiver drops through. */
template <ustore_completion_t completion_>
deferrable<void, completion_> collection_drop_(receiver_t const& receiver, ustore_collection_id_t collection,
                                               ustore_drop_mode_t mode) noexcept {
    return answered_<deferrable<void, completion_>>([&](ustore_outcome_t* outcome, outputs<void>*) noexcept {
        ustore_collection_drop_t arguments {.database = receiver.database,
                                            .executor = receiver.executor,
                                            .outcome = outcome,
                                            .completion = completion_,
                                            .collection = collection,
                                            .mode = mode};
        ustore_collection_drop(&arguments);
    });
}

/** A progress handler: hears the units finished of a total and answers whether the call goes on. */
template <typename handler_type_>
concept a_progress_handler =
    std::is_nothrow_invocable_r_v<ustore_continuation_t, handler_type_&, ustore_size_t, ustore_size_t>;

/** A progress handler the caller keeps for one call, a callback and the state it reads; empty
 *  hears nothing. */
class progress_t {

    template <a_progress_handler handler_type_>
    static ustore_continuation_t report_(ustore_size_t finished, ustore_size_t total, void* state) noexcept {
        return (*static_cast<handler_type_*>(state))(finished, total);
    }

  public:

    /** Reports the advance of the call, or @c nullptr for a call nothing watches. */
    ustore_progress_callback_t callback = nullptr;

    /** The handler that @c callback calls. */
    void* state = nullptr;

    progress_t() noexcept = default;

    /** Views @p handler, which answers @c ustore_cancel_k to stop the call. */
    template <a_progress_handler handler_type_>
    progress_t(handler_type_& handler) noexcept : callback(&report_<handler_type_>), state(&handler) {}
};

#pragma endregion Receivers

} // namespace unum::ustore
