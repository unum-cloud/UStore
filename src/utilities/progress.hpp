/**
 *  @file src/utilities/progress.hpp
 *  @author Ash Vardanian
 *  @date October 13, 2026
 *  @brief The one reporter of a call's progress, which serializes its callbacks and carries a cancellation to every
 *      thread of the call.
 */

#pragma once

#include <cstdint> // `std::uint8_t`

#include <atomic> // `std::atomic`, `std::memory_order_acquire`

#include "ustore/status.hpp" // `status_t`
#include "ustore/types.h"    // `ustore_progress_callback_t`, `ustore_size_t`

namespace unum::ustore::utilities {

/** What a call answers once its progress callback cancels it. */
inline constexpr status_t progress_cancelled_k {status_code_t::cancelled_k, "The progress callback cancelled the call"};

/** Whether the callback of a reporter may run, runs, or cancelled the call. */
enum class reporting_t : std::uint8_t {
    idle_k,
    reporting_k,
    cancelled_k,
};

/**
 *  @brief One serialized reporter per call, so callbacks never run concurrently, and every thread of the call learns
 *      once one of them cancelled it.
 *
 *  A thread finding another one reporting counts its units and skips the callback, which a later report covers, so
 *  the counts a callback sees never decrease. A call reports only while it runs, so no callback runs once its status
 *  is final.
 */
class progress_t {
    ustore_progress_callback_t callback_ = nullptr;
    void* state_ = nullptr;
    std::atomic<ustore_size_t> finished_ {0};
    std::atomic<reporting_t> reporting_ {reporting_t::idle_k};
    ustore_size_t total_ = 0;

  public:
    progress_t(ustore_progress_callback_t callback, void* state, ustore_size_t total) noexcept
        : callback_(callback), state_(state), total_(total) {}

    /** Counts @p units finished and reports when no other thread is; false asks the call to unwind. */
    bool advance(ustore_size_t units) noexcept {
        finished_.fetch_add(units, std::memory_order_relaxed);
        if (!callback_) return true;
        reporting_t observed = reporting_t::idle_k;
        if (!reporting_.compare_exchange_strong(observed, reporting_t::reporting_k, std::memory_order_acquire,
                                                std::memory_order_acquire))
            return observed != reporting_t::cancelled_k;
        bool const carries_on = callback_(finished_.load(std::memory_order_relaxed), total_, state_) != ustore_cancel_k;
        reporting_.store(carries_on ? reporting_t::idle_k : reporting_t::cancelled_k, std::memory_order_release);
        return carries_on;
    }
};

} // namespace unum::ustore::utilities
