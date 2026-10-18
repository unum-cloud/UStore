/**
 *  @file include/ustore/utilities/ram_device.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief The one owner of threads and topology, over ForkUnion's C ABI.
 *
 *  Engines, tools, tests and benchmarks dispatch through @c executor_t, a pool spawned here, or
 *  @c lent_pool_t, a pool another library lends, both through their @c pool_view_t, and only the arenas of
 *  @c utilities/ram_buffers.hpp call @c fu_* directly besides.
 */
#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint32_t`

#include <type_traits> // `std::is_nothrow_invocable_v`, `std::remove_reference_t`
#include <utility>     // `std::exchange`, `std::forward`, `std::swap`

#include <forkunion.h>

#include <smashtable/shared.hpp>

#include "ustore/utilities/types.hpp" // `st::status_t`, `st::expected`

/** Whether one call holds a word that every other call finds contended, global like the arena handle carrying it. */
enum class claim_t : std::uint32_t {
    free_k,
    held_k,
};

/** Where an executor computes, and so where the arenas it makes live, global like the arena handle carrying it. */
enum class residency_t : std::uint8_t {

    /** Host threads over host pages, as every executor of the RAM, RocksDB, LevelDB and NVMe engines computes. */
    host_k,

    /** Host threads driving CUDA devices, as a VRAM executor computes, where no engine places a database yet. */
    vram_k,
};

namespace unum::ustore::utilities {

#pragma region Executor

/** Translates a ForkUnion failure into the status every UStore container reports. */
constexpr st::status_t status_from_(fu_status_t status) noexcept {
    using st::status_t;
    switch (status) {
    case fu_success_k: return status_t::success_k;
    case fu_bad_alloc_k: return status_t::out_of_memory_heap_k;
    case fu_capacity_exhausted_k: return status_t::capacity_exhausted_k;
    case fu_invalid_argument_k: return status_t::invalid_argument_k;
    case fu_thread_refused_k:
    case fu_permission_denied_k: return status_t::operation_not_permitted_k;
    case fu_contended_k: return status_t::operation_would_block_k;
    default: return status_t::unknown_k;
    }
}

/** A callable run once on every thread, with the thread index and optionally its compute domain. */
template <typename callback_>
concept thread_callback = std::is_nothrow_invocable_v<callback_&, std::size_t, std::size_t> ||
                          std::is_nothrow_invocable_v<callback_&, std::size_t>;

/** A callable run once per task, with the task index and the index of the thread running it. */
template <typename callback_>
concept task_callback = std::is_nothrow_invocable_v<callback_&, std::size_t, std::size_t>;

/** What the thread that spawns a pool does on every dispatch it makes afterwards. */
enum class caller_exclusivity_t : std::uint8_t {

    /** Counts as one of the threads, runs its share of every dispatch, and is pinned like the rest. */
    caller_inclusive_k,

    /** Only dispatches and waits, keeping its own affinity and adding nothing to the thread count. */
    caller_exclusive_k,
};

/**
 *  @brief Dispatches on a ForkUnion pool owned elsewhere; copying it copies the right to dispatch, never the pool.
 *
 *  A dispatch claims the pool itself, so one finding the pool running another dispatch, its own or another caller's,
 *  runs nothing and answers @c st::status_t::operation_would_block_k.
 */
class pool_view_t {
  public:
    explicit constexpr pool_view_t(fu_pool_t pool) noexcept : pool_(pool) {}

    /** Runs @p callback once on every thread, with the thread index and, when it takes one, the compute domain. */
    template <thread_callback callback_>
    [[nodiscard]] st::status_t for_threads(callback_&& callback) const noexcept {
        fu_for_threads_t const trampoline = [](fu_lambda_context_t context, std::size_t thread,
                                               std::size_t compute_domain) noexcept {
            auto& target = *static_cast<std::remove_reference_t<callback_>*>(context);
            if constexpr (std::is_nothrow_invocable_v<callback_&, std::size_t, std::size_t>)
                target(thread, compute_domain);
            else target(thread);
        };
        return status_from_(fu_pool_for_threads(pool_, trampoline, &callback));
    }

    /** Runs @p callback over @p count tasks of uneven cost, each thread taking the next task as it finishes one. */
    template <task_callback callback_>
    [[nodiscard]] st::status_t for_n_dynamic(std::size_t count, callback_&& callback) const noexcept {
        fu_for_task_t const trampoline = [](fu_lambda_context_t context, std::size_t task, std::size_t thread,
                                            std::size_t) noexcept {
            (*static_cast<std::remove_reference_t<callback_>*>(context))(task, thread);
        };
        return status_from_(fu_pool_for_n_dynamic(pool_, count, trampoline, &callback));
    }

    /** The number of threads every dispatch runs on, the caller included when it is a worker. */
    [[nodiscard]] std::size_t threads_count() const noexcept {
        std::size_t threads = 0;
        return fu_pool_threads_count(pool_, &threads) == fu_success_k ? threads : 0;
    }

    /** The ForkUnion pool itself, for lending it to a library that borrows pools. */
    [[nodiscard]] fu_pool_t pool() const noexcept { return pool_; }

  private:

    /** The pool every dispatch runs on, owned elsewhere. */
    fu_pool_t pool_;
};

/** The machine as ForkUnion harvests it, its cores, compute domains and memory domains, owned and deleted here. */
class topology_t {
  public:
    topology_t(topology_t&& other) noexcept : topology_(std::exchange(other.topology_, nullptr)) {}

    topology_t& operator=(topology_t&& other) noexcept {
        std::swap(topology_, other.topology_);
        return *this;
    }

    topology_t(topology_t const&) = delete;
    topology_t& operator=(topology_t const&) = delete;

    ~topology_t() noexcept { fu_topology_delete(topology_); }

    /** Harvests the topology of the machine the process runs on. */
    [[nodiscard]] static st::expected<topology_t> make() noexcept {
        topology_t topology;
        if (fu_status_t const status = fu_topology_new(&topology.topology_); status != fu_success_k)
            return status_from_(status);
        return topology;
    }

    /** Counts the memory domains the process may allocate on. */
    [[nodiscard]] std::size_t memory_domains_count() const noexcept {
        std::size_t count = 0;
        return fu_memory_domains_count(topology_, &count) == fu_success_k ? count : 0;
    }

    /** The identifier the operating system gives memory domain @p index, which the allocators take, or -1 past the
     *  last domain. */
    [[nodiscard]] fu_memory_domain_id_t memory_domain(std::size_t index) const noexcept {
        fu_memory_domain_id_t identifier = -1;
        return fu_memory_domain_id_at_index(topology_, index, &identifier) == fu_success_k ? identifier : -1;
    }

    /** Counts the compute domains, the clusters of cores a pool spreads its threads over. */
    [[nodiscard]] std::size_t compute_domains_count() const noexcept {
        std::size_t count = 0;
        return fu_compute_domains_count(topology_, &count) == fu_success_k ? count : 0;
    }

    /** The identifier of the memory domain local to compute domain @p compute_domain. */
    [[nodiscard]] fu_memory_domain_id_t memory_domain_near(std::size_t compute_domain) const noexcept {
        std::size_t index = 0;
        if (fu_local_memory_of(topology_, compute_domain, &index) != fu_success_k) return -1;
        return memory_domain(index);
    }

    /** Counts the logical cores of compute domain @p compute_domain. */
    [[nodiscard]] std::size_t cores_in(std::size_t compute_domain) const noexcept {
        std::size_t cores = 0;
        return fu_logical_cores_count_in(topology_, compute_domain, &cores) == fu_success_k ? cores : 0;
    }

    /** Counts the logical cores this process may run on. */
    [[nodiscard]] std::size_t logical_cores_count() const noexcept {
        std::size_t cores = 0;
        return fu_logical_cores_count(topology_, &cores) == fu_success_k ? cores : 0;
    }

    /** The ForkUnion topology itself, which pools spawn over. */
    [[nodiscard]] fu_topology_t get() const noexcept { return topology_; }

  private:
    constexpr topology_t() noexcept = default;

    /** The harvested topology, owned and deleted here. */
    fu_topology_t topology_ = nullptr;
};

/**
 *  @brief A ForkUnion pool spawned and deleted here, the only kind an executor parks.
 *
 *  A worker caller counts as one of the threads and runs its share of every dispatch, while a
 *  coordinator only waits. Every dispatch blocks until all threads return, and refuses while another one runs.
 */
class executor_t {
  public:

    /** The label the pool's threads carry in the OS. */
    static constexpr char const pool_name_k[] = "ustore";

    executor_t(executor_t&& other) noexcept : pool_(std::exchange(other.pool_, nullptr)) {}

    executor_t& operator=(executor_t&& other) noexcept {
        std::swap(pool_, other.pool_);
        return *this;
    }

    executor_t(executor_t const&) = delete;
    executor_t& operator=(executor_t const&) = delete;

    ~executor_t() noexcept { fu_pool_delete(pool_); }

    /**
     *  @brief Harvests the topology and spawns @p threads threads across the whole machine, the caller among them
     *      for a worker.
     *  @param[in] threads Thread count, at least one; see @c logical_cores_count for the machine's.
     *  @param[in] exclusivity Whether the caller is one of the @p threads or only coordinates them.
     */
    [[nodiscard]] static st::expected<executor_t> make(std::size_t threads, caller_exclusivity_t exclusivity) noexcept {
        st::expected<topology_t> topology = topology_t::make();
        if (!topology) return topology.status();
        return make(*topology, threads, exclusivity);
    }

    /** Spawns @p threads threads interleaved across every compute domain of @p topology. */
    [[nodiscard]] static st::expected<executor_t> make(topology_t const& topology, std::size_t threads,
                                                       caller_exclusivity_t exclusivity) noexcept {
        executor_t executor;
        if (fu_status_t const status = fu_pool_new(pool_name_k, fu_capabilities_all_k, &executor.pool_);
            status != fu_success_k)
            return status_from_(status);
        if (fu_status_t const status = fu_pool_spawn(topology.get(), executor.pool_, threads, spawned_(exclusivity));
            status != fu_success_k)
            return status_from_(status);
        return executor;
    }

    /** Spawns @p threads threads the OS may run on any core, for a thread coordinating a pinned pool, which would
     *  otherwise share a core with that pool's first thread and wait out a scheduler slice behind it. */
    [[nodiscard]] static st::expected<executor_t> make_unpinned(std::size_t threads,
                                                                caller_exclusivity_t exclusivity) noexcept {
        st::expected<topology_t> topology = topology_t::make();
        if (!topology) return topology.status();
        executor_t executor;
        // Without memory placement ForkUnion spawns a flat pool of plain threads, which pins nothing
        fu_capabilities_t const unplaced = static_cast<fu_capabilities_t>(fu_capabilities_all_k &
                                                                          ~fu_capability_place_memory_on_domain_k);
        if (fu_status_t const status = fu_pool_new(pool_name_k, unplaced, &executor.pool_); status != fu_success_k)
            return status_from_(status);
        if (fu_status_t const status = fu_pool_spawn(topology->get(), executor.pool_, threads, spawned_(exclusivity));
            status != fu_success_k)
            return status_from_(status);
        return executor;
    }

    /** Spawns @p threads threads interleaved across the compute domains of @p topology local to @p memory_domain. */
    [[nodiscard]] static st::expected<executor_t> make_near(topology_t const& topology,
                                                            fu_memory_domain_id_t memory_domain, std::size_t threads,
                                                            caller_exclusivity_t exclusivity) noexcept {
        executor_t executor;
        if (fu_status_t const status = fu_pool_new(pool_name_k, fu_capabilities_all_k, &executor.pool_);
            status != fu_success_k)
            return status_from_(status);
        if (fu_status_t const status = fu_pool_spawn_near_memory_domain(topology.get(), executor.pool_, memory_domain,
                                                                        threads, spawned_(exclusivity));
            status != fu_success_k)
            return status_from_(status);
        return executor;
    }

    /** Whether this build spawns OS threads, without which a pool is only the one thread that spawns it. */
    [[nodiscard]] static bool spawns_threads() noexcept {
        return (fu_comptime_capabilities() & fu_capability_os_threads_k) != 0;
    }

    /** Counts the logical cores this process may run on, without spawning anything. */
    [[nodiscard]] static st::expected<std::size_t> logical_cores_count() noexcept {
        st::expected<topology_t> topology = topology_t::make();
        if (!topology) return topology.status();
        return topology->logical_cores_count();
    }

    /** The number of threads every dispatch runs on, the caller included when it is a worker. */
    [[nodiscard]] std::size_t threads_count() const noexcept { return view().threads_count(); }

    /** The right to dispatch on the pool, which stays owned here. */
    [[nodiscard]] pool_view_t view() const noexcept { return pool_view_t {pool_}; }

    /** Puts the idle threads to sleep, each checking for work every @p micros, until the next dispatch wakes them. */
    void park(std::size_t micros) noexcept { fu_pool_sleep(pool_, micros); }

    /**
     *  @brief Starts @c loop_ over @p context on every thread and returns while it runs.
     *  @return The generation @c join waits for; no other dispatch may come before that join, and @p context
     *      outlives it.
     */
    template <auto loop_, typename context_type_>
    [[nodiscard]] st::expected<fu_generation_t> start(context_type_& context) noexcept {
        fu_for_threads_t const trampoline = [](fu_lambda_context_t erased, std::size_t, std::size_t) noexcept {
            loop_(*static_cast<context_type_*>(erased));
        };
        fu_generation_t generation = 0;
        if (fu_status_t const status = fu_pool_unsafe_for_threads(pool_, trampoline, &context, &generation);
            status != fu_success_k)
            return status_from_(status);
        return generation;
    }

    /** Waits until every loop @c start began as @p generation returns. */
    void join(fu_generation_t generation) noexcept { fu_pool_unsafe_join(pool_, generation); }

    /** Runs @p callback once on every thread, with the thread index and, when it takes one, the compute domain. */
    template <thread_callback callback_>
    [[nodiscard]] st::status_t for_threads(callback_&& callback) noexcept {
        return view().for_threads(std::forward<callback_>(callback));
    }

    /** Runs @p callback over @p count similar-cost tasks split evenly across the threads. */
    template <task_callback callback_>
    [[nodiscard]] st::status_t for_n(std::size_t count, callback_&& callback) noexcept {
        fu_for_task_t const trampoline = [](fu_lambda_context_t context, std::size_t task, std::size_t thread,
                                            std::size_t) noexcept {
            (*static_cast<std::remove_reference_t<callback_>*>(context))(task, thread);
        };
        return status_from_(fu_pool_for_n(pool_, count, trampoline, &callback));
    }

  private:
    constexpr executor_t() noexcept = default;

    /** ForkUnion's spelling of @p exclusivity. */
    static constexpr fu_caller_exclusivity_t spawned_(caller_exclusivity_t exclusivity) noexcept {
        return exclusivity == caller_exclusivity_t::caller_inclusive_k ? fu_caller_inclusive_k : fu_caller_exclusive_k;
    }

    /** The pool every dispatch runs on, owned and deleted here, which retains nothing of the topology it spawned
     *  over. */
    fu_pool_t pool_ = nullptr;
};

/** A pool lent by another library: dispatched on during a batch unless its lender's dispatch runs, never deleted,
 *  never parked. */
class lent_pool_t {
  public:

    /** Borrows @p pool, refusing a null pool or one holding no threads. */
    [[nodiscard]] static st::expected<lent_pool_t> borrow(fu_pool_t pool) noexcept {
        if (!pool || pool_view_t {pool}.threads_count() == 0) return st::status_t::invalid_argument_k;
        return lent_pool_t {pool};
    }

    /** The right to dispatch on the pool, which stays with its lender. */
    [[nodiscard]] pool_view_t view() const noexcept { return pool_view_t {pool_}; }

  private:
    explicit constexpr lent_pool_t(fu_pool_t pool) noexcept : pool_(pool) {}

    /** The pool every dispatch runs on, which its lender deletes. */
    fu_pool_t pool_;
};

#pragma endregion Executor

} // namespace unum::ustore::utilities
