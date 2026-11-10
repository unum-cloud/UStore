/**
 *  @file src/utilities/slices.hpp
 *  @author Ash Vardanian
 *  @date November 8, 2026
 *  @brief Splits the batch of one call over the threads of the executor holding it, by words.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include "ustore/status.hpp"                // `status_t`
#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`
#include "ustore/utilities/types.hpp"       // `divide_round_up`

#include "engine.hpp" // `view_t`, `view_of`

namespace unum::ustore::utilities {

/** Tasks per word of a batch, so the presence bits of two slices never share a byte. */
inline constexpr std::size_t tasks_per_word_k = 64;

/** How many slots the slices of @p call index by their thread index: one per pool thread and one
 *  for the call itself, or one for a call without an executor. */
inline std::size_t slice_slots(view_t const& call) noexcept {
    return call.executor ? call.executor->scratches.size() : 1;
}

/**
 *  @brief Runs @p slice over [0, @p count) split into one run of whole words per thread of the
 *      executor of @p call, each with its thread's scratch and index. A call without an
 *      executor, or a batch of one word, runs once on the calling thread with @p call itself.
 *
 *  A thread's scratch rewinds only when the next call starts, so what one slice leaves there
 *  stays readable by the call and by the slices of its later forks.
 *
 *  @param[in] slice Called as <tt>slice(first, length, view)</tt> on runs that never overlap.
 *  @return The first failed slice's status, in task order.
 */
template <typename slice_type_>
status_t for_task_slices(view_t const& call, std::size_t count, slice_type_&& slice) noexcept {
    std::size_t const words = divide_round_up(count, tasks_per_word_k);
    ustore_executor_t const executor = call.executor;
    if (!executor || words <= 1) return count ? slice(std::size_t {0}, count, call) : status_t {};

    pool_view_t const pool = view_of(executor->pool);
    std::size_t const threads = pool.threads_count();
    auto [statuses, statuses_status] = arena_allocate<status_t>(call.scratch, threads);
    if (statuses_status.failed()) return statuses_status;
    for (status_t& status : statuses) status = {};
    auto run_slice = [&](std::size_t thread) noexcept {
        std::size_t const first = thread * words / threads * tasks_per_word_k;
        std::size_t const end = fu::min_of((thread + 1) * words / threads * tasks_per_word_k, count);
        if (first >= end) return;
        view_t sliced = call;
        sliced.scratch = &executor->scratches[thread];
        sliced.thread_index = thread;
        statuses[thread] = slice(first, end - first, sliced);
    };
    if (st::failed(pool.for_threads(run_slice)))
        return {status_code_t::contended_k, "The lent pool runs its lender's dispatch"};
    for (status_t const& status : statuses)
        if (status.failed()) return status;
    return {};
}

} // namespace unum::ustore::utilities
