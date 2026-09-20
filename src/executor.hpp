/**
 *  @file src/executor.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief The one owner of threads, topology and domain-placed memory, over ForkUnion's C ABI.
 *
 *  Nothing else calls @c fu_* directly: engines, tools, tests and benchmarks dispatch through
 *  @c executor_t and place memory through @c domain_allocator.
 */
#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`
#include <cstdlib> // `std::aligned_alloc`, `std::free`

#include <limits>      // `std::numeric_limits`
#include <type_traits> // `std::is_nothrow_invocable_v`, `std::remove_reference_t`, `std::true_type`
#include <utility>     // `std::exchange`, `std::swap`

#include <forkunion.h>

#include <smashtable/shared.hpp>

#include <ustore/types.hpp>

namespace unum::ustore {

#pragma region Placement

/** The memory domain of a machine without NUMA, which the allocator serves from the heap. */
inline constexpr fu_memory_domain_id_t no_memory_domain_k = -1;

/** Places every allocation on one memory domain, and on the heap for @c no_memory_domain_k. */
template <typename value_type_>
class domain_allocator {
  public:
    using value_type = value_type_;
    using propagate_on_container_move_assignment = std::true_type;

    explicit constexpr domain_allocator(fu_memory_domain_id_t memory_domain) noexcept : memory_domain_(memory_domain) {}

    /** The rebinding conversion @c std::allocator_traits performs; the domain travels with it. */
    template <typename other_type_>
    constexpr domain_allocator(domain_allocator<other_type_> const& other) noexcept
        : memory_domain_(other.memory_domain()) {}

    /** Allocates room for @p count values, or answers @c nullptr when that fails or would overflow. */
    [[nodiscard]] value_type_* allocate(std::size_t count) noexcept {
        if (count == 0 || count > max_count_k) return nullptr;
        std::size_t const bytes = aligned_bytes_(count);
        if (memory_domain_ == no_memory_domain_k)
            return static_cast<value_type_*>(std::aligned_alloc(alignment_k, bytes));
        void* memory = nullptr;
        if (fu_allocate_on_domain_id(memory_domain_, bytes, &memory) != fu_success_k) return nullptr;
        return static_cast<value_type_*>(memory);
    }

    /** Releases @p pointer, which must come from @c allocate with the same @p count. */
    void deallocate(value_type_* pointer, std::size_t count) noexcept {
        if (memory_domain_ == no_memory_domain_k) return std::free(pointer);
        fu_free_on_domain_id(memory_domain_, pointer, aligned_bytes_(count));
    }

    /** The OS memory-domain id storage lands on. */
    constexpr fu_memory_domain_id_t memory_domain() const noexcept { return memory_domain_; }

    template <typename other_type_>
    constexpr bool operator==(domain_allocator<other_type_> const& other) const noexcept {
        return memory_domain_ == other.memory_domain();
    }

  private:

    /** The cache-line width both the heap and the domain paths align to. */
    static constexpr std::size_t alignment_k = 64;

    /** The largest count whose aligned byte size still fits a @c std::size_t. */
    static constexpr std::size_t max_count_k = (std::numeric_limits<std::size_t>::max() - alignment_k) /
                                               sizeof(value_type_);

    /** Rounds up to @c alignment_k, as @c std::aligned_alloc requires and both paths share. */
    static constexpr std::size_t aligned_bytes_(std::size_t count) noexcept {
        return (count * sizeof(value_type_) + alignment_k - 1) / alignment_k * alignment_k;
    }

    fu_memory_domain_id_t memory_domain_;
};

#pragma endregion Placement

#pragma region Executor

/** A callable run once on every thread, with the thread index and optionally its compute domain. */
template <typename callback_>
concept thread_callback = std::is_nothrow_invocable_v<callback_&, std::size_t, std::size_t> ||
                          std::is_nothrow_invocable_v<callback_&, std::size_t>;

/** A callable run once per task, with the task index and the index of the thread running it. */
template <typename callback_>
concept task_callback = std::is_nothrow_invocable_v<callback_&, std::size_t, std::size_t>;

/** What the thread that spawns a pool does on every dispatch it makes afterwards. */
enum class caller_role_t : std::uint8_t {

    /** Counts as one of the threads, runs its share of every dispatch, and is pinned like the rest. */
    worker_k,

    /** Only dispatches and waits, keeping its own affinity and adding nothing to the thread count. */
    coordinator_k,
};

/**
 *  @brief A ForkUnion pool spawned over the machine topology it owns.
 *
 *  A worker caller counts as one of the threads and runs its share of every dispatch, while a
 *  coordinator only waits. Every dispatch blocks until all threads return, and admits one caller at a time.
 */
class executor_t {
  public:

    /** The label the pool's threads carry in the OS. */
    static constexpr char const pool_name_k[] = "ustore";

    executor_t(executor_t&& other) noexcept
        : topology_(std::exchange(other.topology_, nullptr)), pool_(std::exchange(other.pool_, nullptr)) {}

    executor_t& operator=(executor_t&& other) noexcept {
        std::swap(topology_, other.topology_);
        std::swap(pool_, other.pool_);
        return *this;
    }

    executor_t(executor_t const&) = delete;
    executor_t& operator=(executor_t const&) = delete;

    ~executor_t() noexcept {
        fu_pool_delete(pool_);
        fu_topology_delete(topology_);
    }

    /**
     *  @brief Harvests the topology and spawns @p threads threads, the caller among them for a worker.
     *  @param[in] threads Thread count, at least one; see @c logical_cores_count for the machine's.
     *  @param[in] role Whether the caller is one of the @p threads or only coordinates them.
     */
    [[nodiscard]] static st::expected<executor_t> make(std::size_t threads, caller_role_t role) noexcept {
        executor_t executor;
        if (fu_status_t const status = fu_topology_new(&executor.topology_); status != fu_success_k)
            return status_from_(status);
        if (fu_status_t const status = fu_pool_new(pool_name_k, fu_capabilities_all_k, &executor.pool_);
            status != fu_success_k)
            return status_from_(status);
        fu_caller_exclusivity_t const exclusivity = role == caller_role_t::worker_k ? fu_caller_inclusive_k
                                                                                    : fu_caller_exclusive_k;
        if (fu_status_t const status = fu_pool_spawn(executor.topology_, executor.pool_, threads, exclusivity);
            status != fu_success_k)
            return status_from_(status);
        return executor;
    }

    /** Counts the logical cores this process may run on, without spawning anything. */
    [[nodiscard]] static st::expected<std::size_t> logical_cores_count() noexcept {
        fu_topology_t topology = nullptr;
        if (fu_status_t const status = fu_topology_new(&topology); status != fu_success_k) return status_from_(status);
        std::size_t cores = 0;
        fu_status_t const status = fu_logical_cores_count(topology, &cores);
        fu_topology_delete(topology);
        if (status != fu_success_k) return status_from_(status);
        return cores;
    }

    /** The number of threads every dispatch runs on, the caller included when it is a worker. */
    [[nodiscard]] std::size_t threads_count() const noexcept {
        std::size_t threads = 0;
        return fu_pool_threads_count(pool_, &threads) == fu_success_k ? threads : 0;
    }

    /** The memory domain nearest @p compute_domain, or @c no_memory_domain_k without NUMA. */
    [[nodiscard]] fu_memory_domain_id_t memory_domain_near(std::size_t compute_domain) const noexcept {
        std::size_t memory_domain_index = 0;
        fu_memory_domain_id_t memory_domain = no_memory_domain_k;
        if (fu_local_memory_of(topology_, compute_domain, &memory_domain_index) != fu_success_k ||
            fu_memory_domain_id_at_index(topology_, memory_domain_index, &memory_domain) != fu_success_k)
            return no_memory_domain_k;
        return memory_domain;
    }

    /** Runs @p callback once on every thread, with the thread index and, when it takes one, the compute domain. */
    template <thread_callback callback_>
    [[nodiscard]] st::status_t for_threads(callback_&& callback) noexcept {
        fu_for_threads_t const trampoline = [](fu_lambda_context_t context, std::size_t thread,
                                               std::size_t compute_domain) noexcept {
            auto& target = *static_cast<std::remove_reference_t<callback_>*>(context);
            if constexpr (std::is_nothrow_invocable_v<callback_&, std::size_t, std::size_t>)
                target(thread, compute_domain);
            else target(thread);
        };
        return status_from_(fu_pool_for_threads(pool_, trampoline, &callback));
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

    /** Translates a ForkUnion failure into the status every UStore container reports. */
    static constexpr st::status_t status_from_(fu_status_t status) noexcept {
        using st::status_t;
        switch (status) {
        case fu_success_k: return status_t::success_k;
        case fu_bad_alloc_k: return status_t::out_of_memory_heap_k;
        case fu_capacity_exhausted_k: return status_t::capacity_exhausted_k;
        case fu_invalid_argument_k: return status_t::invalid_argument_k;
        case fu_thread_refused_k:
        case fu_permission_denied_k: return status_t::operation_not_permitted_k;
        default: return status_t::unknown_k;
        }
    }

    /** The machine topology the pool was spawned over, owned and deleted here. */
    fu_topology_t topology_ = nullptr;

    /** The pool every dispatch runs on, owned and deleted here. */
    fu_pool_t pool_ = nullptr;
};

#pragma endregion Executor

} // namespace unum::ustore
