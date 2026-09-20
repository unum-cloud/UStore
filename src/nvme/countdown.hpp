/**
 *  @file src/nvme/countdown.hpp
 *  @author Ash Vardanian
 *  @date September 17, 2026
 *  @brief The completion counter a fanned-out job reports through, and the handle its parts hold.
 */

#pragma once

#include <cstdint> // `std::int32_t`

#include <atomic>  // `std::memory_order`
#include <thread>  // `std::this_thread::yield`
#include <utility> // `std::exchange`

#include <forkunion/atomics.hpp> // `fu::preferred_atomic_ref`

#include <ustore/status.hpp> // `status_t`, `status_code_t`
#include <ustore/types.hpp>  // the `fu` alias

namespace unum::ustore::nvme {

/** Hands the core back to the scheduler, which is what a wait with no work of its own does. */
struct thread_yield_t {
    void operator()() const noexcept { std::this_thread::yield(); }
};

/**
 *  @brief The parts still owed by one fanned-out job, and the first failure any of them reported.
 *
 *  One owner holds the counter and waits on it; every part holds a @ref notifier_t onto it and
 *  reports through that, so the counter itself is never copied and outlives every part.
 */
class countdown_t {
  public:

    countdown_t() noexcept = default;

    /** A counter owed @p parts reports. */
    explicit countdown_t(std::size_t parts) noexcept : remaining_(static_cast<std::int32_t>(parts)) {}

    countdown_t(countdown_t&&) = default;
    countdown_t& operator=(countdown_t&&) = default;
    countdown_t(countdown_t const&) = delete;
    countdown_t& operator=(countdown_t const&) = delete;

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
     *  @brief Waits until every part reports or one fails, running @p task between polls.
     *
     *  @param[in] task What the waiting thread does while it waits, which is the executor's own
     *      work where there is any; the default hands the core back to the scheduler.
     *  @return Whether every part succeeded.
     */
    template <typename background_task_type_ = thread_yield_t>
    bool wait(background_task_type_&& task = {}) noexcept {
        while (!has_finished()) task();
        return !has_failed();
    }

    /** The failure a part reported, and success once the counter carries none again. */
    status_t release_status() noexcept {
        int const code = code_reference_().exchange(0, std::memory_order_acquire);
        if (code == 0) return {};
        return {static_cast<status_code_t>(code), std::exchange(message_, nullptr)};
    }

  private:

    friend class notifier_t;

    /** The parts still owed, which a failure drives to zero however many are outstanding. */
    std::int32_t remaining_ = 1;

    /** The @c ustore_status_t of the first failure, or zero while none has been reported. */
    int code_ = 0;

    /** The static message of the first failure, stored before the code that publishes it. */
    char const* message_ = nullptr;

    fu::preferred_atomic_ref<std::int32_t> remaining_reference_() noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {remaining_};
    }

    fu::preferred_atomic_ref<std::int32_t const> remaining_reference_() const noexcept {
        return fu::preferred_atomic_ref<std::int32_t const> {remaining_};
    }

    fu::preferred_atomic_ref<int> code_reference_() noexcept { return fu::preferred_atomic_ref<int> {code_}; }

    fu::preferred_atomic_ref<int const> code_reference_() const noexcept {
        return fu::preferred_atomic_ref<int const> {code_};
    }
};

/**
 *  @brief One part's handle onto the counter its job reports through, cheap enough to copy per part.
 *
 *  Every handle borrows a counter that outlives it, so a part reports without knowing who waits.
 */
class notifier_t {
  public:

    notifier_t() = delete;
    notifier_t(notifier_t const&) = default;

    /** A handle onto @p countdown, which must outlive every part holding one. */
    notifier_t(countdown_t& countdown) noexcept : countdown_(countdown) {}

    /** The counter this handle reports through. */
    countdown_t& countdown() const noexcept { return countdown_; }

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

} // namespace unum::ustore::nvme
