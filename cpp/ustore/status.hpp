/**
 *  @file cpp/ustore/status.hpp
 *  @author Ash Vardanian
 *  @date July 4, 2022
 *  @brief Statuses with static messages, the expected wrapper that every fallible C++ call returns, and the deferred
 *      call that a call submitted to an executor answers.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <concepts>    // `std::convertible_to`, `std::integral`, `std::same_as`
#include <new>         // placement `new`, `std::nothrow`
#include <tuple>       // `std::apply`, `std::tuple`, `std::tuple_size`, `std::tuple_element`
#include <type_traits> // `std::is_nothrow_move_constructible_v`, `std::remove_cvref_t`, `std::type_identity`
#include <utility>     // `std::cmp_equal`, `std::exchange`, `std::move`, `std::swap`

#include "ustore/types.h" // `ustore_status_t`, `ustore_outcome_t`

namespace unum::ustore {

/** What went wrong, with one enumerator per @c ustore_status_t that the C header documents. */
enum class status_code_t : int {
    success_k = ustore_status_success_k,
    deferred_k = ustore_status_deferred_k,
    out_of_memory_k = ustore_status_out_of_memory_k,
    invalid_argument_k = ustore_status_invalid_argument_k,
    uninitialized_k = ustore_status_uninitialized_k,
    unsupported_k = ustore_status_unsupported_k,
    io_k = ustore_status_io_k,
    malformed_k = ustore_status_malformed_k,
    not_found_k = ustore_status_not_found_k,
    already_exists_k = ustore_status_already_exists_k,
    write_conflict_k = ustore_status_write_conflict_k,
    read_conflict_k = ustore_status_read_conflict_k,
    phantom_conflict_k = ustore_status_phantom_conflict_k,
    capacity_exhausted_k = ustore_status_capacity_exhausted_k,
    device_k = ustore_status_device_k,
    internal_k = ustore_status_internal_k,
    contended_k = ustore_status_contended_k,
    cancelled_k = ustore_status_cancelled_k,
    read_only_k = ustore_status_read_only_k,
};

/** The C++ twin of @c ustore_outcome_t: a code and a static message, @c nullptr on success. */
struct [[nodiscard]] status_t {

    /** What went wrong, or @c status_code_t::success_k. */
    status_code_t code = status_code_t::success_k;

    /** Static detail the code cannot carry, never freed. */
    char const* message = nullptr;

    constexpr status_t() noexcept = default;
    constexpr status_t(status_code_t reason, char const* detail = nullptr) noexcept : code(reason), message(detail) {}
    constexpr status_t(ustore_outcome_t const& outcome) noexcept
        : code(static_cast<status_code_t>(outcome.status)), message(outcome.message) {}

    /** The same status as the C ABI spells it, for writing into an outcome. */
    constexpr operator ustore_outcome_t() const noexcept { return {static_cast<ustore_status_t>(code), message}; }

    constexpr bool succeeded() const noexcept { return code == status_code_t::success_k; }
    constexpr bool failed() const noexcept { return code != status_code_t::success_k; }

    friend constexpr bool operator==(status_t const& status, status_code_t code) noexcept {
        return status.code == code;
    }
};

static_assert(std::is_trivially_copyable_v<status_t>);

/**
 *  @brief A value, or the status that explains its absence.
 *
 *  The value lives in a @c union that is alive only on success, so @p value_type_ needs no default constructor and
 *  may be move-only. Decomposes as @c auto @c [value, status] where the value is nothrow default constructible.
 */
template <typename value_type_>
class [[nodiscard]] expected {
  public:
    using value_t = value_type_;
    using value_type = value_type_;

    static_assert(std::is_nothrow_move_constructible_v<value_t>, "expected moves its value without throwing");

  private:
    union {
        value_t value_;
    };
    status_t status_;

  public:

    /** A failure; a success that carries no value reads as @c status_code_t::internal_k. */
    constexpr expected(status_t status) noexcept
        : status_(status.succeeded() ? status_t {status_code_t::internal_k, "An expected value is missing"} : status) {}

    /** A failure without a message. */
    constexpr expected(status_code_t code) noexcept : expected(status_t {code}) {}

    expected(value_t&& value) noexcept { new (&value_) value_t(std::move(value)); }

    expected(value_t const& value) noexcept
        requires std::is_nothrow_copy_constructible_v<value_t>
    {
        new (&value_) value_t(value);
    }

    expected(expected&& other) noexcept : status_(other.status_) {
        if (status_.succeeded()) new (&value_) value_t(std::move(other.value_));
    }

    expected(expected const& other) noexcept
        requires std::is_nothrow_copy_constructible_v<value_t>
        : status_(other.status_) {
        if (status_.succeeded()) new (&value_) value_t(other.value_);
    }

    expected& operator=(expected&& other) noexcept {
        if (this == &other) return *this;
        if (status_.succeeded()) value_.~value_t();
        status_ = other.status_;
        if (status_.succeeded()) new (&value_) value_t(std::move(other.value_));
        return *this;
    }

    expected& operator=(expected const& other) noexcept
        requires std::is_nothrow_copy_constructible_v<value_t>
    {
        if (this == &other) return *this;
        if (status_.succeeded()) value_.~value_t();
        status_ = other.status_;
        if (status_.succeeded()) new (&value_) value_t(other.value_);
        return *this;
    }

    ~expected() noexcept {
        if (status_.succeeded()) value_.~value_t();
    }

    constexpr explicit operator bool() const noexcept { return status_.succeeded(); }
    constexpr bool has_value() const noexcept { return status_.succeeded(); }
    constexpr bool failed() const noexcept { return status_.failed(); }
    constexpr status_t status() const noexcept { return status_; }
    constexpr status_code_t code() const noexcept { return status_.code; }

    /** @warning Reading the value of a failed @c expected is undefined, so check it first. */
    constexpr value_t& operator*() & noexcept { return value_; }
    constexpr value_t const& operator*() const& noexcept { return value_; }
    constexpr value_t&& operator*() && noexcept { return std::move(value_); }
    constexpr value_t* operator->() noexcept { return &value_; }
    constexpr value_t const* operator->() const noexcept { return &value_; }

    /** The tuple protocol behind @c auto @c [value, status], default-constructing the value of a failure. */
    template <std::size_t index_>
        requires std::is_nothrow_default_constructible_v<value_t>
    constexpr auto get() && noexcept {
        if constexpr (index_ == 0) return status_.succeeded() ? value_t {std::move(value_)} : value_t {};
        else return status_;
    }

    template <std::size_t index_>
        requires(std::is_nothrow_default_constructible_v<value_t> && std::is_nothrow_copy_constructible_v<value_t>)
    constexpr auto get() const& noexcept {
        if constexpr (index_ == 0) return status_.succeeded() ? value_t {value_} : value_t {};
        else return status_;
    }
};

/** Whether @p result carries @p code, which is @c status_code_t::success_k exactly when it holds a value. */
template <typename value_type_>
constexpr bool operator==(expected<value_type_> const& result, status_code_t code) noexcept {
    return result.code() == code;
}

/** Whether @p result holds a value equal to @p value, so a failure equals nothing. */
template <typename value_type_, typename comparable_type_>
    requires(!std::same_as<std::remove_cvref_t<comparable_type_>, status_code_t>) &&
            requires(value_type_ const& held, comparable_type_ const& other) {
                { held == other } -> std::convertible_to<bool>;
            }
constexpr bool operator==(expected<value_type_> const& result, comparable_type_ const& value) noexcept {
    if (!result.has_value()) return false;
    if constexpr (std::integral<value_type_> && std::integral<comparable_type_> && !std::same_as<value_type_, bool> &&
                  !std::same_as<comparable_type_, bool>)
        return std::cmp_equal(*result, value);
    else return *result == value;
}

/**
 *  @brief A call submitted to an executor, owning the one heap block that holds its outcome, every plain input it
 *      reads and every output it writes through, which never moves until the call is final.
 *
 *  Submission copies the C arguments, so they live only on the stack of @c submit, while everything they point to lives
 *  in the block. An arena is a handle the call borrows, since freeing it waits for the call. @c status polls without
 *  blocking, and @c wait blocks until the call is final and hands over what it produced. The destructor waits too, so
 *  nothing the call reads goes away under it, and a block that could not be allocated reads as a call that finished
 *  with @c status_code_t::out_of_memory_k.
 *
 *  @tparam verb_type_ What one verb keeps beside its inputs: the outputs the call writes through, its C
 *      @c arguments_t, the static @c submit that hands them to the library, and @c finish, which turns the outputs of
 *      a successful call into its @c answer_t.
 */
template <typename verb_type_>
class [[nodiscard]] deferred {
  public:
    using verb_t = verb_type_;
    using arguments_t = typename verb_type_::arguments_t;
    using answer_t = typename verb_type_::answer_t;

  private:
    struct block_t {
        ustore_outcome_t outcome {};
        verb_type_ verb {};
        void (*destroy)(block_t*) noexcept = nullptr;
    };

    template <typename... inputs_types_>
    struct owning_block : block_t {
        std::tuple<inputs_types_...> inputs;
        explicit owning_block(inputs_types_&&... owned) noexcept : inputs(std::move(owned)...) {}
    };

    block_t* block_ = nullptr;

    explicit deferred(block_t* block) noexcept : block_(block) {}

    void release_() noexcept {
        if (!block_) return;
        ustore_outcome_wait(&block_->outcome);
        block_->destroy(block_);
        block_ = nullptr;
    }

  public:
    deferred(deferred&& other) noexcept : block_(std::exchange(other.block_, nullptr)) {}
    deferred& operator=(deferred&& other) noexcept {
        std::swap(block_, other.block_);
        return *this;
    }
    deferred(deferred const&) = delete;
    deferred& operator=(deferred const&) = delete;
    ~deferred() noexcept { release_(); }

    /**
     *  @brief Moves @p inputs into a new block, lets @p prepare fill the C arguments from them, and submits a copy.
     *  @param[in] prepare Fills the C arguments from the verb and the owned inputs, pointing them only into the block,
     *      and answers a status that becomes the final one, with nothing submitted, when it fails.
     *  @param[in] inputs Everything the call reads, which the block owns until the call is final.
     */
    template <typename prepare_type_, typename... inputs_types_>
    static deferred submit(prepare_type_&& prepare, inputs_types_... inputs) noexcept {
        using owning_block_t = owning_block<inputs_types_...>;
        owning_block_t* const block = new (std::nothrow) owning_block_t(std::move(inputs)...);
        if (!block) return deferred {nullptr};
        block->destroy = [](block_t* erased) noexcept { delete static_cast<owning_block_t*>(erased); };
        arguments_t arguments {};
        status_t const prepared = std::apply(
            [&](inputs_types_&... owned) noexcept { return prepare(block->verb, arguments, owned...); }, block->inputs);
        if (prepared.failed()) block->outcome = prepared;
        else {
            arguments.outcome = &block->outcome;
            arguments.completion = ustore_completion_deferred_k;
            verb_type_::submit(arguments);
        }
        return deferred {block};
    }

    /** The status of the call, polled through @c ustore_outcome_status: @c status_code_t::deferred_k until final. */
    status_code_t status() const noexcept {
        if (!block_) return status_code_t::out_of_memory_k;
        return static_cast<status_code_t>(ustore_outcome_status(&block_->outcome));
    }

    /** Blocks until the call is final, then answers what it produced, or the status explaining why it did not. */
    answer_t wait() && noexcept {
        if (!block_) return status_t {status_code_t::out_of_memory_k, "Failed to allocate a deferred call"};
        auto const code = static_cast<status_code_t>(ustore_outcome_wait(&block_->outcome));
        status_t const status {code, block_->outcome.message};
        answer_t answer = status.failed() ? answer_t {status} : std::move(block_->verb).finish();
        release_();
        return answer;
    }
};

} // namespace unum::ustore

template <typename value_type_>
struct std::tuple_size<unum::ustore::expected<value_type_>> {
    static constexpr std::size_t value = 2;
};

template <typename value_type_>
struct std::tuple_element<0, unum::ustore::expected<value_type_>> {
    using type = value_type_;
};

template <typename value_type_>
struct std::tuple_element<1, unum::ustore::expected<value_type_>> : std::type_identity<unum::ustore::status_t> {};
