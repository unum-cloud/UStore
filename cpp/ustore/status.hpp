/**
 *  @file cpp/ustore/status.hpp
 *  @author Ash Vardanian
 *  @date July 4, 2022
 *  @brief Statuses with static messages and the expected wrapper that every fallible C++ call
 *      returns, re-exported from <tt>ustore/utilities/types.hpp</tt>, and the deferred call that a
 *      call submitted to an executor answers.
 *
 *  Every verb is written once, as a template on its completion: a blocking receiver answers an @c
 *  outcome, the status alone or an @c expected value, and a deferred one answers a @c deferred call
 *  its one owner polls or waits on.
 */

#pragma once

#include <concepts>    // `std::invocable`
#include <type_traits> // `std::conditional_t`, `std::is_void_v`

#include "ustore/types.h"             // `ustore_outcome_t`, `ustore_outcome_wait`, `ustore_completion_t`
#include "ustore/utilities/types.hpp" // `status_code_t`, `status_t`, `expected`, re-exported into `unum::ustore`

namespace unum::ustore {

/** Whether a receiver completed as @p completion_ submits its calls and answers @c deferred ones,
 *  rather than blocking until each is final. */
template <ustore_completion_t completion_>
inline constexpr bool defers = completion_ == ustore_completion_deferred_k;

/** Where a call writes what @p answer_type_ is made of: the answer itself, unless a view needs the
 *  C-shaped fields a specialization lays out beside an @c answer that assembles the view. */
template <typename answer_type_>
struct outputs {

    /** The answer, which the call writes in place. */
    answer_type_ value {};

    answer_type_ answer() const noexcept { return value; }
};

/** A call that outputs nothing but its status. */
template <>
struct outputs<void> {};

/** What a blocking call answers: its status alone, or a value beside it. */
template <typename answer_type_>
using outcome = std::conditional_t<std::is_void_v<answer_type_>, status_t, expected<answer_type_>>;

/** The status of a final call that outputs nothing else. */
inline status_t answered_(ustore_outcome_t const& reported, outputs<void> const&) noexcept {
    return status_t {reported};
}

/** The answer of a final call, or the status explaining why it has none. */
template <typename answer_type_>
    requires(!std::is_void_v<answer_type_>)
expected<answer_type_> answered_(ustore_outcome_t const& reported, outputs<answer_type_> const& written) noexcept {
    if (status_t const status {reported}; status.failed()) return status;
    return written.answer();
}

/**
 *  @brief A submitted call: its outcome and its outputs, owned until the call is final, while the
 *      receiver, the inputs, the arena and the handlers it was handed stay viewed until then.
 *
 *  It never moves, as the library writes into it, and its one owner polls @c status or blocks in @c
 *  wait; destruction waits the same way, so nothing the call writes into goes away under it.
 */
template <typename answer_type_ = void>
class [[nodiscard]] deferred {
    ustore_outcome_t outcome_ {};
    outputs<answer_type_> outputs_ {};

  public:
    /** Runs @p submit with this call's outcome and outputs, where the library reports. */
    template <typename submit_type_>
        requires std::invocable<submit_type_ const&, ustore_outcome_t*, outputs<answer_type_>*>
    explicit deferred(submit_type_ const& submit) noexcept {
        submit(&outcome_, &outputs_);
    }

    /** A call refused before it crossed, final from the start. */
    explicit deferred(status_t refused) noexcept : outcome_(refused) {}

    deferred(deferred const&) = delete;
    deferred& operator=(deferred const&) = delete;
    ~deferred() noexcept { ustore_outcome_wait(&outcome_); }

    /** @c status_code_t::deferred_k until the call is final, then its final status, polled without
     *      blocking. */
    status_code_t status() noexcept { return static_cast<status_code_t>(ustore_outcome_status(&outcome_)); }

    /** Blocks until the call is final and answers what a blocking call would have. */
    outcome<answer_type_> wait() noexcept {
        ustore_outcome_wait(&outcome_);
        return answered_(outcome_, outputs_);
    }
};

/** A blocking call's outcome, or a deferred call the caller holds, as @p completion_ says. */
template <ustore_completion_t completion_, typename answer_type_>
using answer = std::conditional_t<defers<completion_>, deferred<answer_type_>, outcome<answer_type_>>;

/** Runs @p submit with an outcome and outputs: on the stack when blocking, inside the answered
 *  @c deferred otherwise. Every verb's one body fills its C arguments inside @p submit and names
 *  @p completion_ there. */
template <ustore_completion_t completion_, typename answer_type_, typename submit_type_>
answer<completion_, answer_type_> called_(submit_type_ const& submit) noexcept {
    if constexpr (defers<completion_>) return deferred<answer_type_>(submit);
    else {
        ustore_outcome_t reported {};
        outputs<answer_type_> written {};
        submit(&reported, &written);
        return answered_(reported, written);
    }
}

/** Answers a call refused before it crossed, where only the binding sees two inputs disagree. */
template <ustore_completion_t completion_, typename answer_type_>
answer<completion_, answer_type_> refused_(status_t status) noexcept {
    if constexpr (defers<completion_>) return deferred<answer_type_>(status);
    else return outcome<answer_type_>(status);
}

} // namespace unum::ustore
