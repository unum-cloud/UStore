/**
 *  @file src/nvme/resources.hpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Budgets, threads and disks one database shares, and the gate its background jobs pass.
 */

#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::int32_t`

#include <atomic>   // `std::memory_order`
#include <memory>   // `std::shared_ptr`, `std::make_shared`
#include <new>      // placement `new`
#include <optional> // `std::optional`
#include <utility>  // `std::forward`, `std::move`

#include <forkunion/atomics.hpp> // `fu::preferred_atomic_ref`

#include "executor.hpp" // `executor_t`, `caller_exclusivity_t`, `task_callback`, `thread_callback`

#include "nvme/config.hpp"     // `resources_config_t`
#include "nvme/filesystem.hpp" // `mounted_storage_t`, `create_directories`
#include "nvme/types.hpp"      // `resource_constraint_t`, `constrained_allocator`

namespace unum::ustore::nvme {

#pragma region Background Jobs

/**
 *  @brief The flag one owner raises to stop the background jobs it submitted.
 *
 *  An LSM tree flushing its write buffer, a level merger compacting and a backup manager copying
 *  files each hold one, so a job always has a named owner that starts it, observes it and stops
 *  it. Every part of a running job polls @c cancelled between units of work, and the gate refuses
 *  a fresh job whose owner has already raised the flag.
 */
class job_cancellation_t {
  public:

    job_cancellation_t() noexcept = default;
    job_cancellation_t(job_cancellation_t const&) = delete;
    job_cancellation_t& operator=(job_cancellation_t const&) = delete;

    /** Stops the jobs this owner submitted, and refuses the ones it has not submitted yet. */
    void cancel() noexcept { flag_reference_().store(1, std::memory_order_release); }

    /** Admits this owner's jobs again, for a component reopened in place. */
    void resume() noexcept { flag_reference_().store(0, std::memory_order_release); }

    /** Whether the owner has stopped its jobs, which every part of a running one polls. */
    bool cancelled() const noexcept { return flag_reference_().load(std::memory_order_acquire) != 0; }

  private:

    /** Zero while the owner wants its jobs, one once it has cancelled them. */
    std::int32_t flag_ = 0;

    fu::preferred_atomic_ref<std::int32_t> flag_reference_() noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {flag_};
    }

    fu::preferred_atomic_ref<std::int32_t const> flag_reference_() const noexcept {
        return fu::preferred_atomic_ref<std::int32_t const> {flag_};
    }
};

/** Where the pool of a @c background_executor_t stands between its making and its first job. */
enum class pool_state_t : std::int32_t {

    /** No thread exists yet, and the next job admitted spawns them. */
    dormant_k = 0,

    /** One submitter is spawning the threads, and every other one spins until it is done. */
    spawning_k = 1,

    /** The threads are up and every job fans out over them. */
    spawned_k = 2,
};

/**
 *  @brief The executor every background job of one database runs on, with the bound on jobs in flight.
 *
 *  There are no fibers and no repeating loops: an owner calls @c submit_for_n or @c submit_for_threads
 *  when a threshold it tracks is crossed, the work fans out across the pool, and the call returns once
 *  every part has finished. The gate admits a job only while fewer than @c jobs_limit are running, so
 *  thresholds crossing at once queue at their owners rather than in the pool, whose dispatch admits one
 *  caller at a time. The first job admitted spawns the pool, and every submitter only coordinates it,
 *  so a mounted database that never crosses a threshold owns no threads and pins none.
 */
class background_executor_t {
  public:

    background_executor_t(background_executor_t&&) noexcept = default;
    background_executor_t& operator=(background_executor_t&&) noexcept = default;
    background_executor_t(background_executor_t const&) = delete;
    background_executor_t& operator=(background_executor_t const&) = delete;

    /**
     *  @brief Sets the thread count, spawning nothing yet, and admits @p jobs_limit jobs at a time.
     *  @param[in] threads Thread count, at least one, which the first job admitted spawns.
     *  @param[in] jobs_limit Jobs that may be in flight at once, at least one.
     */
    static expected<background_executor_t> make(std::size_t threads, std::size_t jobs_limit) noexcept {
        if (threads == 0 || jobs_limit == 0)
            return status_t {status_code_t::invalid_argument_k, "A background executor needs a thread and a job"};
        return background_executor_t {threads, static_cast<std::int32_t>(jobs_limit)};
    }

    /** The number of threads a job fans out over, which the submitting thread is never among. */
    [[nodiscard]] std::size_t threads_count() const noexcept { return threads_; }

    /** Whether the pool is still to be spawned, being spawned by a submitter right now, or up. */
    [[nodiscard]] pool_state_t pool_state() const noexcept {
        return static_cast<pool_state_t>(pool_state_reference_().load(std::memory_order_acquire));
    }

    /** The jobs running right now, which never passes the limit the gate was made with. */
    [[nodiscard]] std::size_t jobs_in_flight() const noexcept {
        return static_cast<std::size_t>(jobs_in_flight_reference_().load(std::memory_order_acquire));
    }

    /** The jobs that may run at once. */
    [[nodiscard]] std::size_t jobs_limit() const noexcept { return static_cast<std::size_t>(jobs_limit_); }

    /**
     *  @brief Runs @p callback over @p count parts of @p owner's job, split evenly across the threads.
     *  @param[in] owner The flag the job's owner cancels it through, which the gate reads before admitting it.
     *  @param[in] count Parts of the job, which may be zero for a job with nothing to do.
     *  @param[in] callback Runs once per part, with the part index and the index of the thread running it.
     */
    template <task_callback callback_>
    status_t submit_for_n(job_cancellation_t const& owner, std::size_t count, callback_&& callback) noexcept {
        if (status_t const admitted = admit_(owner); admitted.failed()) return admitted;
        st::status_t const dispatched = executor_->for_n(count, std::forward<callback_>(callback));
        retire_();
        return to_status(dispatched);
    }

    /**
     *  @brief Runs @p callback once on every thread of @p owner's job.
     *  @param[in] owner The flag the job's owner cancels it through, which the gate reads before admitting it.
     *  @param[in] callback Runs once per thread, with the thread index and, when it takes one, the compute domain.
     */
    template <thread_callback callback_>
    status_t submit_for_threads(job_cancellation_t const& owner, callback_&& callback) noexcept {
        if (status_t const admitted = admit_(owner); admitted.failed()) return admitted;
        st::status_t const dispatched = executor_->for_threads(std::forward<callback_>(callback));
        retire_();
        return to_status(dispatched);
    }

  private:

    background_executor_t(std::size_t threads, std::int32_t jobs_limit) noexcept
        : threads_(threads), jobs_limit_(jobs_limit) {}

    /** Takes one of the job slots, spawning the pool for the first, or reports why this job does not run. */
    status_t admit_(job_cancellation_t const& owner) noexcept {
        if (owner.cancelled()) return {status_code_t::uninitialized_k, "The owner cancelled its background jobs"};
        if (status_t const spawned = spawn_(); spawned.failed()) return spawned;
        std::int32_t const observed = jobs_in_flight_reference_().fetch_add_if_at_most(1, jobs_limit_,
                                                                                       std::memory_order_acquire);
        if (observed + 1 > jobs_limit_)
            return {status_code_t::capacity_exhausted_k, "Too many background jobs are already in flight"};
        return {};
    }

    /** Hands the job slot back, so the next threshold crossing is admitted. */
    void retire_() noexcept { jobs_in_flight_reference_().sub(1, std::memory_order_release); }

    /** Spawns the pool once, on the first job, spinning out a spawn another submitter has under way. */
    status_t spawn_() noexcept {
        constexpr std::int32_t dormant_k = static_cast<std::int32_t>(pool_state_t::dormant_k);
        constexpr std::int32_t spawning_k = static_cast<std::int32_t>(pool_state_t::spawning_k);
        constexpr std::int32_t spawned_k = static_cast<std::int32_t>(pool_state_t::spawned_k);
        fu::preferred_atomic_ref<std::int32_t> const state = pool_state_reference_();
        std::int32_t observed = dormant_k;
        while (
            !state.compare_exchange_weak(observed, spawning_k, std::memory_order_acquire, std::memory_order_acquire)) {
            if (observed == spawned_k) return {};
            observed = dormant_k;
        }
        st::expected<executor_t> executor = executor_t::make(threads_, caller_exclusivity_t::caller_exclusive_k);
        if (!executor) {
            state.store(dormant_k, std::memory_order_release);
            return to_status(executor.status());
        }
        executor_.emplace(std::move(*executor));
        state.store(spawned_k, std::memory_order_release);
        return {};
    }

    fu::preferred_atomic_ref<std::int32_t> jobs_in_flight_reference_() noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {jobs_in_flight_};
    }

    fu::preferred_atomic_ref<std::int32_t const> jobs_in_flight_reference_() const noexcept {
        return fu::preferred_atomic_ref<std::int32_t const> {jobs_in_flight_};
    }

    fu::preferred_atomic_ref<std::int32_t> pool_state_reference_() noexcept {
        return fu::preferred_atomic_ref<std::int32_t> {pool_state_};
    }

    fu::preferred_atomic_ref<std::int32_t const> pool_state_reference_() const noexcept {
        return fu::preferred_atomic_ref<std::int32_t const> {pool_state_};
    }

    /** The threads every job fans out over, never parked, since @c fu_pool_sleep races a concurrent @c admit_. */
    std::optional<executor_t> executor_;

    /** The threads the first job spawns, which @c threads_count answers before and after the spawn. */
    std::size_t threads_ = 1;

    /** A @c pool_state_t moved by @c spawn_ alone, by compare-exchange out of @c dormant_k. */
    std::int32_t pool_state_ = 0;

    /** The jobs running right now, raised by @c admit_ and lowered by @c retire_. */
    std::int32_t jobs_in_flight_ = 0;

    /** The jobs that may run at once, fixed when the gate is made. */
    std::int32_t jobs_limit_ = 1;
};

using background_executor_shared_ptr_t = std::shared_ptr<background_executor_t>;

#pragma endregion Background Jobs

#pragma region Shared Resources

/** Budgets and services every collection of one database shares. */
struct resources_t {

    /** Budget of host memory. */
    resource_constraint_t memory_constraint;

    /** Budget of GPU memory, zero when no GPU is used. */
    resource_constraint_t gpu_memory_constraint;

    /** GPU devices compaction may use. */
    st::basic_vector<int> gpu_devices;

    /** Threads and the job gate every flush, compaction and backup passes. */
    background_executor_shared_ptr_t executor;

    /** The transport, the pinned arena, the channels and the volumes mounted over the data directories. */
    mounted_storage_t storage;

    /** Sets the memory budgets, leaving the services for the caller to attach. */
    explicit resources_t(std::size_t memory_limit, std::size_t gpu_memory_limit = 0) noexcept
        : memory_constraint(memory_limit), gpu_memory_constraint(gpu_memory_limit) {}
};

using resources_shared_ptr_t = std::shared_ptr<resources_t>;

/** Constructs @p object_ in memory charged to the host budget, or returns an empty pointer once it runs out. */
template <typename object_, typename... arguments_>
std::shared_ptr<object_> make_constrained_shared(resources_shared_ptr_t const& resources, arguments_&&... arguments) {
    constrained_allocator<byte_t> allocator {resources->memory_constraint};
    auto* const pointer = reinterpret_cast<object_*>(allocator.allocate(sizeof(object_)));
    if (!pointer) return {};

    auto deleter = [allocator](object_* constructed) mutable {
        constructed->~object_();
        allocator.deallocate(reinterpret_cast<byte_t*>(constructed), sizeof(object_));
    };
    return std::shared_ptr<object_>(new (pointer) object_(std::forward<arguments_>(arguments)...), deleter);
}

/** The jobs one database admits at once: one per data directory, one for its metadata and ten for compaction. */
inline std::size_t background_jobs_limit(std::size_t data_directories_count) noexcept {
    return data_directories_count + 1 + 10;
}

/**
 *  @brief Creates the metadata directory, opens the executor and mounts the storage of one database.
 *  @param[in] config Budgets, threads, the metadata directory and the data directories to mount.
 *  @param[in] database The database the volumes belong to, or an invalid identifier to take any.
 *  @return The resources, whose destructor tears the mount down in the order it was built in.
 */
inline expected<resources_shared_ptr_t> open_resources(resources_config_t const& config, uuid_t const& database) {
    if (config.directory.empty()) return status_t {status_code_t::invalid_argument_k, "A database needs a directory"};
    if (status_t const made = create_directories(config.directory.c_str()); made.failed()) return made;

    resources_shared_ptr_t resources = std::make_shared<resources_t>(config.memory_limit, config.gpu_memory_limit);
    if (resources->gpu_devices.resize(config.gpu_devices.size()) != st::status_t::success_k)
        return status_t {status_code_t::out_of_memory_k, "A database could not list the GPUs it may use"};
    for (std::size_t index = 0; index != config.gpu_devices.size(); ++index)
        resources->gpu_devices[index] = config.gpu_devices[index];

    std::size_t threads = config.threads;
    if (threads == 0) {
        st::expected<std::size_t> const cores = executor_t::logical_cores_count();
        if (!cores) return to_status(cores.status());
        threads = derived_background_threads(*cores);
    }
    expected<background_executor_t> executor = background_executor_t::make(
        threads, background_jobs_limit(config.data_directories.size()));
    if (!executor) return executor.status();
    resources->executor = std::make_shared<background_executor_t>(std::move(*executor));

    if (status_t const mounted = resources->storage.mount(config, resources->memory_constraint,
                                                          resources->executor->threads_count(), database);
        mounted.failed())
        return mounted;
    return resources;
}

#pragma endregion Shared Resources

} // namespace unum::ustore::nvme
