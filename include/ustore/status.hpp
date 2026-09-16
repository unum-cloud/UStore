/**
 *  @file include/ustore/status.hpp
 *  @author Ash Vardanian
 *  @date July 4, 2022
 *  @brief Statuses with static messages, and the expected wrapper that every fallible C++ call returns.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include <concepts>    // `std::convertible_to`, `std::integral`, `std::same_as`
#include <new>         // placement `new`
#include <tuple>       // `std::tuple_size`, `std::tuple_element`
#include <type_traits> // `std::is_nothrow_move_constructible_v`, `std::remove_cvref_t`, `std::type_identity`
#include <utility>     // `std::cmp_equal`, `std::move`

#include <ustore/db.h> // `ustore_status_t`, `ustore_error_t`

namespace unum::ustore {

/** What went wrong, with one enumerator per @c ustore_status_t that the C header documents. */
enum class status_code_t : int {
    success_k = ustore_success_k,
    out_of_memory_k = ustore_error_out_of_memory_k,
    invalid_argument_k = ustore_error_invalid_argument_k,
    uninitialized_k = ustore_error_uninitialized_k,
    unsupported_k = ustore_error_unsupported_k,
    io_k = ustore_error_io_k,
    malformed_k = ustore_error_malformed_k,
    not_found_k = ustore_error_not_found_k,
    already_exists_k = ustore_error_already_exists_k,
    write_conflict_k = ustore_error_write_conflict_k,
    read_conflict_k = ustore_error_read_conflict_k,
    phantom_conflict_k = ustore_error_phantom_conflict_k,
    capacity_exhausted_k = ustore_error_capacity_exhausted_k,
    device_k = ustore_error_device_k,
    internal_k = ustore_error_internal_k,
};

/** The C++ twin of @c ustore_error_t: a code and a static message, @c nullptr on success. */
struct [[nodiscard]] status_t {

    /** What went wrong, or @c status_code_t::success_k. */
    status_code_t code = status_code_t::success_k;

    /** Static detail the code cannot carry, never freed. */
    char const* message = nullptr;

    constexpr status_t() noexcept = default;
    constexpr status_t(status_code_t reason, char const* detail = nullptr) noexcept : code(reason), message(detail) {}
    constexpr status_t(ustore_error_t const& error) noexcept
        : code(static_cast<status_code_t>(error.status)), message(error.message) {}

    /** The same status as the C ABI spells it, for writing into an argument struct. */
    constexpr operator ustore_error_t() const noexcept { return {static_cast<ustore_status_t>(code), message}; }

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
