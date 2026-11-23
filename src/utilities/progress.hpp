/**
 *  @file src/utilities/progress.hpp
 *  @author Ash Vardanian
 *  @date October 13, 2026
 *  @brief The one reporter of a call's progress, which serializes its callbacks and holds a
 *      cancellation to every thread of the call, and the ticks and driver of a plan's phases.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`

#include "ustore/status.hpp" // `status_t`
#include "ustore/types.h"    // `ustore_progress_callback_t`, `ustore_size_t`, `ustore_phase_progress_t`

#include "engine.hpp" // `floor_atomic_ref`, `phase_t`, `plan_verbs`, `view_t`, `view_of`

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
    ustore_size_t finished_ = 0;
    reporting_t reporting_ = reporting_t::idle_k;
    ustore_size_t total_ = 0;

  public:
    progress_t(ustore_progress_callback_t callback, void* state, ustore_size_t total) noexcept
        : callback_(callback), state_(state), total_(total) {}

    /** Counts @p units finished and reports when no other thread is; false asks the call to unwind. */
    bool advance(ustore_size_t units) noexcept {
        floor_atomic_ref<ustore_size_t> const finished {finished_};
        finished.fetch_add(units, std::memory_order_relaxed);
        if (!callback_) return true;
        floor_atomic_ref<reporting_t> const reporting {reporting_};
        reporting_t observed = reporting_t::idle_k;
        if (!reporting.compare_exchange_strong(observed, reporting_t::reporting_k, std::memory_order_acquire,
                                               std::memory_order_acquire))
            return observed != reporting_t::cancelled_k;
        bool const keeps_going = callback_(finished.load(std::memory_order_relaxed), total_, state_) != ustore_cancel_k;
        reporting.store(keeps_going ? reporting_t::idle_k : reporting_t::cancelled_k, std::memory_order_release);
        return keeps_going;
    }
};

#pragma region Plans

/** One plan call's reports: the C callback, which only the thread driving the plan calls, at each
 *  barrier with that phase's own units, so it never runs concurrently with itself. */
struct progress_ticks_t {
    ustore_progress_callback_t callback = nullptr;
    void* state = nullptr;

    /** Units finished as last reported. */
    std::size_t done = 0;

    /** The population the last report named. */
    std::size_t reported_population = 0;

    /** The tick of a barrier that finished @p finished of its phase's @p population units; the
     *  callback hears every barrier that finished more, or some of another population. */
    ustore_continuation_t operator()(std::size_t finished, std::size_t population) noexcept {
        if (!callback) return ustore_continue_k;
        if (finished == done && (population == reported_population || !finished)) return ustore_continue_k;
        done = finished;
        reported_population = population;
        return callback(finished, population, state);
    }
};

/** A range over an indexed accessor: @c owner[position] for every position below @p count,
 *  so a caller reads <tt>for (auto element : range)</tt> rather than counting positions itself. */
template <typename owner_type_>
struct indexed_range {

    /** What the positions are read from; viewed, never owned. */
    owner_type_ const* owner = nullptr;

    /** Positions the range covers, from zero. */
    std::size_t count = 0;

    constexpr std::size_t size() const noexcept { return count; }
    constexpr bool empty() const noexcept { return !count; }
    constexpr auto operator[](std::size_t position) const noexcept { return (*owner)[position]; }

    struct iterator {
        owner_type_ const* owner = nullptr;
        std::size_t position = 0;

        constexpr auto operator*() const noexcept { return (*owner)[position]; }
        constexpr iterator& operator++() noexcept { return ++position, *this; }
        constexpr bool operator==(iterator const& other) const noexcept { return position == other.position; }
        constexpr bool operator!=(iterator const& other) const noexcept { return position != other.position; }
    };
    constexpr iterator begin() const noexcept { return {owner, 0}; }
    constexpr iterator end() const noexcept { return {owner, count}; }
};

/** The C spelling of where an engine's @p plan stands. */
template <typename plan_type_>
ustore_phase_progress_t phase_of(plan_type_ const& plan) noexcept {
    // Every engine phase is valued as its C spelling, so a reorder on either side fails to compile
    static_assert(static_cast<ustore_phase_t>(phase_t::none_k) == ustore_phase_none_k);
    static_assert(static_cast<ustore_phase_t>(phase_t::flush_k) == ustore_phase_flush_k);
    static_assert(static_cast<ustore_phase_t>(phase_t::merge_k) == ustore_phase_merge_k);
    static_assert(static_cast<ustore_phase_t>(phase_t::reclaim_k) == ustore_phase_reclaim_k);
    static_assert(static_cast<ustore_phase_t>(phase_t::done_k) == ustore_phase_done_k);
    return {static_cast<ustore_phase_t>(plan.phase()), plan.ordinal(), plan.completed(), plan.total()};
}

/**
 *  @brief Runs @p plan 's pending phases until it ends or @p ticks cancels: each phase's tasks,
 *      then its barrier, with a tick of the phase's units.
 *
 *  With an executor, a phase of several tasks fans them out over its pool, each on its thread's
 *  scratch, and one task runs on the call's thread, free to fan its own work out; without one, the
 *  tasks run in turn on the calling thread. A failed task starts no further one, and the phase
 *  runs again from its first task on the next call, since its tasks are idempotent.
 */
template <typename plan_type_>
status_t run_plan(void* plan, view_t const& runner, progress_ticks_t& ticks) noexcept {
    plan_type_& typed = *static_cast<plan_type_*>(plan);
    ustore_executor_t const executor = runner.executor;
    while (true) {
        auto const tasks = typed.tasks();
        if (!executor || tasks.size() <= 1) {
            status_t ran;
            for (std::size_t position = 0; position != tasks.size() && ran.succeeded(); ++position)
                arena_rewind(runner.scratch), ran = typed.run(tasks[position], runner);
            if (ran.failed()) return ran;
        }
        else {
            status_t first;
            bool failing = false;
            auto run_task = [&](std::size_t position, std::size_t thread) noexcept {
                floor_atomic_ref<bool> const failed_once {failing};
                if (failed_once.load(std::memory_order_acquire)) return;
                view_t const view {.scratch = &executor->scratches[thread],
                                   .executor = executor,
                                   .thread_index = thread};
                arena_rewind(view.scratch);
                status_t const ran = typed.run(tasks[position], view);
                bool unfailed = false;
                if (ran.failed() && failed_once.compare_exchange_strong(unfailed, true, std::memory_order_acq_rel))
                    first = ran;
            };
            if (st::failed(executor->pool.view().for_n_dynamic(tasks.size(), run_task)))
                return {status_code_t::internal_k, "The executor's idle pool refused a fork"};
            if (first.failed()) return first;
        }
        std::size_t const completed = typed.completed(), total = typed.total();
        bool const following = typed.advance();
        if (status_t const met = typed.status(); met.failed()) return met;
        if (total && ticks(completed, total) == ustore_cancel_k) return progress_cancelled_k;
        if (!following) return {};
    }
}

template <typename plan_type_>
ustore_phase_progress_t plan_phase(void const* plan) noexcept {
    return phase_of(*static_cast<plan_type_ const*>(plan));
}

template <typename plan_type_>
status_t finish_plan(void* plan) noexcept {
    return static_cast<plan_type_*>(plan)->finish();
}

template <typename plan_type_>
void free_plan(void* plan) noexcept {
    delete static_cast<plan_type_*>(plan);
}

/** The entries every plan of @p plan_type_ runs through. */
template <typename plan_type_>
constexpr plan_verbs plan_verbs_for() noexcept {
    return {&run_plan<plan_type_>, &plan_phase<plan_type_>, &finish_plan<plan_type_>, &free_plan<plan_type_>};
}

#pragma endregion Plans

} // namespace unum::ustore::utilities
