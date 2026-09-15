/**
 *  @file include/ustore/status.hpp
 *  @author Ash Vardanian
 *  @date July 4, 2022
 *  @brief Owning error statuses and the expected wrapper that every fallible C++ call returns.
 */

#pragma once

#include <type_traits> // `std::is_integral_v`
#include <utility>     // `std::cmp_equal`, `std::exchange`, `std::move`, `std::swap`

#include <ustore/types.hpp>

namespace unum::ustore {

/** An error message from the C ABI that frees itself, where no message means success. */
class [[nodiscard]] status_t {

    /** Whether the destructor frees the message or leaves it to a static owner. */
    enum class ownership_t : bool { borrowed_k, owned_k };

    ustore_error_t message_ {nullptr};
    ownership_t ownership_ {ownership_t::owned_k};

  public:

    /** Wraps a static message that must never reach @c ustore_error_free. */
    static status_t status_view(ustore_error_t message) noexcept {
        status_t status {message};
        status.ownership_ = ownership_t::borrowed_k;
        return status;
    }

    status_t(ustore_error_t message = nullptr) noexcept : message_(message) {}

    status_t(status_t const&) = delete;
    status_t& operator=(status_t const&) = delete;

    status_t(status_t&& other) noexcept
        : message_(std::exchange(other.message_, nullptr)),
          ownership_(std::exchange(other.ownership_, ownership_t::owned_k)) {}

    status_t& operator=(status_t&& other) noexcept {
        std::swap(message_, other.message_);
        std::swap(ownership_, other.ownership_);
        return *this;
    }

    ~status_t() noexcept {
        if (message_ && ownership_ == ownership_t::owned_k) ustore_error_free(message_);
        message_ = nullptr;
    }

    operator bool() const noexcept { return !message_; }

    ustore_error_t* member_ptr() noexcept { return &message_; }
    ustore_error_t release_error() noexcept { return std::exchange(message_, nullptr); }
    ustore_error_t message() const noexcept { return message_; }
};

/** A value together with the status that explains its absence. */
template <typename value_>
class [[nodiscard]] expected {
    status_t status_;
    value_ object_;

  public:
    expected() = default;
    expected(value_&& object) noexcept : object_(std::move(object)) {}
    expected(status_t&& status, value_&& fallback = value_ {}) noexcept
        : status_(std::move(status)), object_(std::move(fallback)) {}

    expected(expected&&) noexcept = default;
    expected& operator=(expected&&) noexcept = default;

    operator bool() const noexcept { return status_; }

    value_& operator*() & noexcept { return object_; }
    value_ const& operator*() const& noexcept { return object_; }
    value_ operator*() && noexcept { return std::move(object_); }
    value_* operator->() noexcept { return &object_; }
    value_ const* operator->() const noexcept { return &object_; }

    status_t release_status() noexcept { return std::exchange(status_, status_t {}); }

    /** Whether the value is present and equals @p other. */
    template <typename other_type_>
    bool operator==(other_type_ const& other) const noexcept {
        if (!status_) return false;
        if constexpr (std::is_integral_v<value_> && std::is_integral_v<other_type_>)
            return std::cmp_equal(object_, other);
        else return object_ == other;
    }
};

/** Error categories the engines report. */
enum error_code_t {
    success_k = 0,
    out_of_memory_k,
    out_of_range_k,
    args_combo_k,
    args_wrong_k,
    uninitialized_state_k,
    network_k,
    consistency_k,
    missing_feature_k,
    error_unknown_k
};

} // namespace unum::ustore
