/**
 *  @file src/utilities/slices.hpp
 *  @author Ash Vardanian
 *  @date November 8, 2026
 *  @brief Splits the work of one call into tasks, or runs of words, which the threads of
 *      its executor claim.
 */

#pragma once

#include <cstddef> // `std::size_t`

#include "ustore/utilities/ram_buffers.hpp" // `arena_allocate`
#include "ustore/utilities/types.hpp"       // `status_t`, `divide_round_up`

#include "engine.hpp" // `view_t`

namespace unum::ustore::utilities {

/** Tasks per word of a batch, so the presence bits of two slices never share a byte. */
inline constexpr std::size_t tasks_per_word_k = 64;

/** How many slots the runs of @p call index: one per pool thread and one for the call itself, or
 *  one for a call without an executor. */
inline std::size_t slice_slots(view_t const& call) noexcept {
    return call.executor ? call.executor->scratches.size() : 1;
}

/**
 *  @brief Runs @p task once per index below @p count on the threads of the executor of @p call,
 *      in one fork, each thread claiming the next index with its own scratch and thread index.
 *
 *  A call without an executor runs every task on the calling thread with @p call itself.
 *  A thread's scratch rewinds when the next call starts, so what one task leaves there stays
 *  readable by the call and by the tasks of its later forks. A call may also rewind its
 *  tasks' scratches between forks, once nothing the tasks left is read again, but never the
 *  call's own scratch.
 *
 *  @param[in] task Called as <tt>task(index, view)</tt>, concurrently from the executor's threads.
 *  @return The first failed task's status, in task order.
 */
template <typename task_type_>
status_t for_tasks(view_t const& call, std::size_t count, task_type_&& task) noexcept {
    ustore_executor_t const executor = call.executor;
    if (!executor) {
        for (std::size_t index = 0; index != count; ++index)
            if (status_t const status = task(index, call); status.failed()) return status;
        return {};
    }
    if (!count) return {};
    auto [statuses, statuses_status] = arena_allocate<status_t>(call.scratch, count);
    if (statuses_status.failed()) return statuses_status;
    for (status_t& status : statuses) status = {};
    std::size_t next = 0;
    auto claim = [&](std::size_t thread) noexcept {
        view_t sliced = call;
        sliced.scratch = &executor->scratches[thread];
        sliced.thread_index = thread;
        floor_atomic_ref<std::size_t> claims {next};
        for (std::size_t index = claims.fetch_add(1, std::memory_order_relaxed); index < count;
             index = claims.fetch_add(1, std::memory_order_relaxed))
            statuses[index] = task(index, sliced);
    };
    if (fu::failed(executor->threads.for_threads(claim)))
        return {status_code_t::internal_k, "The executor's idle pool refused a fork"};
    for (status_t const& status : statuses)
        if (status.failed()) return status;
    return {};
}

/**
 *  @brief Runs @p slice over [0, @p count) cut into runs of whole words, at most one per thread of
 *      the executor of @p call, which its threads claim as @c for_tasks claims tasks, or one run
 *      on the calling thread for a call without an executor.
 *
 *  @param[in] slice Called as <tt>slice(first, length, run, view)</tt> on runs that never overlap,
 *      @p run below @c slice_slots, which indexes what each run fills.
 *  @return The first failed slice's status, in run order.
 */
template <typename slice_type_>
status_t for_task_slices(view_t const& call, std::size_t count, slice_type_&& slice) noexcept {
    if (!count) return {};
    std::size_t const words = divide_round_up(count, tasks_per_word_k);
    std::size_t const runs = call.executor ? fu::min_of(words, call.executor->threads.threads_count()) : 1;
    return for_tasks(call, runs, [&](std::size_t run, view_t const& sliced) noexcept -> status_t {
        std::size_t const first = run * words / runs * tasks_per_word_k;
        std::size_t const end = fu::min_of((run + 1) * words / runs * tasks_per_word_k, count);
        return first < end ? slice(first, end - first, run, sliced) : status_t {};
    });
}

} // namespace unum::ustore::utilities
