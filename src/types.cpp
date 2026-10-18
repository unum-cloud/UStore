/**
 *  @file src/types.cpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief Implements `c/ustore/types.h`: outcomes, the spelling of statuses and moods, the version, executors with
 *      their drivers, and arenas.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`

#include <algorithm>   // `std::find`, `std::max`, `std::reverse`
#include <array>       // `std::array`
#include <atomic>      // `std::atomic_ref`, `std::memory_order_release`
#include <bit>         // `std::bit_ceil`, `std::bit_cast`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <type_traits> // `std::underlying_type_t`
#include <utility>     // `std::move`
#include <variant>     // `std::variant`, `std::get_if`

#include <smashtable/basic_vector.hpp> // `st::basic_vector`

#include "ustore/types.h" // `ustore_executor_for_ram_t`, `ustore_arena_init_t`, `ustore_version`

#if USTORE_WITH_CUDA
#include "ustore/utilities/vram_nvidia_device.hpp" // `cuda_count_devices`
#endif

#include "lib.hpp" // `call_executor_t`, `finish`, `run_as`, `wait_finished`, `enumerator_spelling`

namespace unum::ustore {

#if USTORE_WITH_CUDA

/** Bytes of device memory a VRAM executor's scratch takes on each device when its creation names none. */
constexpr std::size_t vram_scratch_limit_default_k = std::size_t {256} << 20;
#endif

/** The canonical spelling of every status, indexed by its value. */
constexpr std::array<std::string_view, ustore_status_read_only_k + 1> status_names_k {
    "success",
    "deferred",
    "out-of-memory",
    "invalid-argument",
    "uninitialized",
    "unsupported",
    "io",
    "malformed",
    "not-found",
    "already-exists",
    "write-conflict",
    "read-conflict",
    "phantom-conflict",
    "capacity-exhausted",
    "device",
    "internal",
    "contended",
    "cancelled",
    "read-only",
};

/** The canonical spelling of every mood. */
constexpr enumerator_spelling<ustore_mood_t> mood_names_k[] {
    {ustore_mood_parked_k, "parked"},
    {ustore_mood_spinning_k, "spinning"},
};

/** The canonical spelling of every transport. */
constexpr enumerator_spelling<ustore_transport_t> transport_names_k[] {
    {ustore_transport_automatic_k, "automatic"},     {ustore_transport_blocking_k, "blocking"},
    {ustore_transport_linux_uring_k, "linux-uring"}, {ustore_transport_linux_aio_k, "linux-aio"},
    {ustore_transport_linux_spdk_k, "linux-spdk"},
};

/** The version as "major.minor.patch", spelled from the version macros at compile time. */
constexpr auto version_name_k = [] {
    std::array<char, 3 * 11 + 3> text {};
    std::size_t length = 0;
    for (int part : {USTORE_VERSION_MAJOR, USTORE_VERSION_MINOR, USTORE_VERSION_PATCH}) {
        if (length) text[length++] = '.';
        std::size_t const first = length;
        do text[length++] = static_cast<char>('0' + part % 10);
        while (part /= 10);
        std::reverse(text.begin() + first, text.begin() + length);
    }
    return text;
}();

/** Finishes the outcomes of the first @p count results of @p executor, each before lowering the deferred counts its
 *  call raised, so a free waiting on one of them returns after that outcome is final. */
void publish_results(call_executor_t& executor, std::uint32_t count) noexcept {
    for (std::uint32_t task = 0; task != count; ++task) {
        deferred_result_t const& result = executor.results[task];
        finish(*result.outcome, result.status);
        for (std::uint32_t* const deferred_calls : result.deferred_counts) {
            if (!deferred_calls) continue;
            std::atomic_ref<std::uint32_t> const calls {*deferred_calls};
            calls.fetch_sub(1, std::memory_order_release);
            calls.notify_all();
        }
    }
}

/** Runs the ready slots from @c head whose tickets follow one another as one batch, answering how many it ran.
 *
 *  The batch's table positions are freed before any of its outcomes is final, so a caller seeing one final may submit
 *  again at once. Nothing reads a slot after that, since a submission may refill it. */
std::uint32_t serve_batch(call_executor_t& executor) noexcept {
    std::uint32_t const head = std::atomic_ref<std::uint32_t> {executor.head}.load(std::memory_order_relaxed);
    std::size_t const mask = executor.deferred.size() - 1;
    deferred_slot_t& first = executor.deferred[head & mask];
    if (std::atomic_ref<std::uint32_t> {first.ready}.load(std::memory_order_acquire) != head + 1) return 0;
    std::uint32_t const first_ticket = first.ticket;
    std::uint32_t count = 1;
    for (; count != executor.deferred_limit; ++count) {
        deferred_slot_t& slot = executor.deferred[(head + count) & mask];
        if (std::atomic_ref<std::uint32_t> {slot.ready}.load(std::memory_order_acquire) != head + count + 1 ||
            slot.ticket != first_ticket + count)
            break;
    }

    wait_finished(executor, first_ticket);
    auto serve_task = [&executor, head, mask](std::size_t task, std::size_t thread) noexcept {
        deferred_slot_t& slot = executor.deferred[(head + task) & mask];
        run_as(executor, thread,
               [&](view_t const& runner) noexcept { slot.run(&slot.arguments, executor.results[task], runner); });
    };
    // A lent pool running its lender's dispatch refuses ours and runs nothing, so the driver runs the batch itself
    if (st::failed(view_of(executor.pool).for_n_dynamic(count, serve_task)))
        for (std::uint32_t task = 0; task != count; ++task) serve_task(task, executor.runners.size() - 1);
    std::atomic_ref<std::uint32_t> {executor.head}.store(head + count, std::memory_order_release);
    publish_results(executor, count);
    advance_finished(executor, first_ticket + count);
    return count;
}

/** Serves the deferred table of @p executor on its driver thread until @c ustore_executor_free stops it. */
void drive(call_executor_t& executor) noexcept {
    std::atomic_ref<std::uint32_t> const doorbell {executor.doorbell};
    for (;;) {
        std::uint32_t const rung = doorbell.load(std::memory_order_acquire);
        if (serve_batch(executor) != 0) continue;
        if (std::atomic_ref<driver_state_t> {executor.state}.load(std::memory_order_acquire) ==
            driver_state_t::stopping_k)
            return;
        if (std::atomic_ref<ustore_mood_t> {executor.mood}.load(std::memory_order_relaxed) == ustore_mood_spinning_k)
            continue;
        if (executor_t* const owned = std::get_if<executor_t>(&executor.pool)) owned->park(parked_check_micros_k);
        doorbell.wait(rung, std::memory_order_acquire);
    }
}

/** How a pool spawned here treats the thread spawning it: apart from its workers, or as its only thread in a build
 *  without OS threads. */
caller_exclusivity_t spawned_exclusivity() noexcept {
    return executor_t::spawns_threads() ? caller_exclusivity_t::caller_exclusive_k
                                        : caller_exclusivity_t::caller_inclusive_k;
}

/** Explains why an executor could not have its threads. */
status_t pool_failure(st::status_t status) noexcept {
    switch (status) {
    case st::status_t::out_of_memory_heap_k: return {status_code_t::out_of_memory_k, "Out of memory for the threads"};
    case st::status_t::invalid_argument_k: return {status_code_t::invalid_argument_k, "The pool holds no threads"};
    case st::status_t::operation_not_permitted_k:
        return {status_code_t::unsupported_k, "The system refused to start the threads"};
    default: return {status_code_t::internal_k, "ForkUnion failed for an unknown reason"};
    }
}

/** Reads into @p domains the memory domains @p options name, refusing one @p topology lacks, or every domain the
 *  process may allocate on when they name none. */
status_t read_memory_domains(ustore_ram_options_t const& options, topology_t const& topology,
                             st::basic_vector<fu_memory_domain_id_t>& domains) noexcept {
    std::size_t const available = topology.memory_domains_count();
    bool const derived = !options.memory_domains || !options.memory_domains_count;
    std::size_t const count = derived ? available : static_cast<std::size_t>(options.memory_domains_count);
    if (!count) return {status_code_t::unsupported_k, "The machine reports no memory domain"};
    if (st::failed(domains.resize(count))) return {status_code_t::out_of_memory_k, "Failed to copy the memory domains"};
    for (std::size_t index = 0; index != count; ++index) {
        if (derived) {
            domains[index] = topology.memory_domain(index);
            continue;
        }
        fu_memory_domain_id_t const named = options.memory_domains[index];
        bool known = false;
        for (std::size_t candidate = 0; candidate != available && !known; ++candidate)
            known = topology.memory_domain(candidate) == named;
        if (!known) return {status_code_t::invalid_argument_k, "No memory domain carries the identifier"};
        domains[index] = named;
    }
    return {};
}

/** Whether @p domains hold @p domain. */
bool serves(std::span<fu_memory_domain_id_t const> domains, fu_memory_domain_id_t domain) noexcept {
    return std::find(domains.begin(), domains.end(), domain) != domains.end();
}

/** Counts the cores of the compute domains of @p topology local to any of @p domains. */
std::size_t cores_near(topology_t const& topology, std::span<fu_memory_domain_id_t const> domains) noexcept {
    std::size_t cores = 0;
    for (std::size_t compute_domain = 0; compute_domain != topology.compute_domains_count(); ++compute_domain)
        if (serves(domains, topology.memory_domain_near(compute_domain))) cores += topology.cores_in(compute_domain);
    return cores;
}

nvme_placement_t nvme_placement_of(ustore_executor_t executor) noexcept {
    nvme_placement_t placement;
    if (!executor || !executor->moves_nvme) return placement;
    placement.nvme = executor->nvme;
    placement.threads = view_of(executor->pool).threads_count();
    placement.executor = executor;
    if (call_executor_t const* const vram = executor->vram) {
        placement.devices = std::span<std::int32_t const> {vram->devices};
        placement.scratch_limit = vram->scratch_limit;
    }
    return placement;
}

} // namespace unum::ustore

/** Makes in @p made the executor running calls on @p pool, with its driver and a table of @p deferred_limit calls,
 *  keeping the scratch of every thread of the pool on the memory domain of @p topology local to it when @p domains
 *  hold it, and every other scratch and arena on the first of them. */
static status_t make_executor(std::variant<executor_t, lent_pool_t> pool, topology_t const& topology,
                              std::span<fu_memory_domain_id_t const> domains, std::size_t deferred_limit,
                              ustore_executor_t& made) noexcept {
    std::size_t const limit = deferred_limit ? deferred_limit : deferred_limit_default_k;
    if (limit > deferred_limit_max_k)
        return {status_code_t::capacity_exhausted_k, "An executor holds at most 2^30 deferred calls"};
    st::expected<executor_t> driver = executor_t::make_unpinned(1, spawned_exclusivity());
    if (!driver) return pool_failure(driver.status());
    std::size_t const threads = view_of(pool).threads_count();
    ustore_executor_handle_t* const executor = new (std::nothrow)
        ustore_executor_handle_t {{std::move(pool), std::move(*driver)}};
    if (!executor) return {status_code_t::out_of_memory_k, "Failed to allocate the executor"};
    executor->deferred_limit = static_cast<std::uint32_t>(limit);
    if (st::failed(executor->deferred.resize(std::bit_ceil(limit))) || st::failed(executor->results.resize(limit)) ||
        st::failed(executor->runners.resize(threads + 1))) {
        delete executor;
        return {status_code_t::out_of_memory_k, "Failed to allocate the deferred table"};
    }

    executor->memory_domain = domains.front();
    for (runner_t& runner : executor->runners) runner.scratch.memory_domain = domains.front();
    // A lent pool running its lender's dispatch refuses this one, leaving every scratch on the first domain
    [[maybe_unused]] st::status_t const placed =
        view_of(executor->pool).for_threads([&](std::size_t thread, std::size_t compute_domain) noexcept {
            fu_memory_domain_id_t const local = topology.memory_domain_near(compute_domain);
            if (serves(domains, local)) executor->runners[thread].scratch.memory_domain = local;
        });

    if (!executor_t::spawns_threads()) executor->state = driver_state_t::absent_k;
    else if (st::expected<fu_generation_t> started = executor->driver.start<&drive>(*executor); started)
        executor->generation = *started;
    else {
        delete executor;
        return pool_failure(started.status());
    }
    made = executor;
    return {};
}

ustore_status_t ustore_outcome_wait(ustore_outcome_t* outcome) {
    std::atomic_ref<ustore_status_t> const word {outcome->status};
    for (;;) {
        ustore_status_t const seen = word.load(std::memory_order_acquire);
        if (seen != ustore_status_deferred_k) return seen;
        word.wait(seen, std::memory_order_acquire);
    }
}

ustore_status_t ustore_outcome_status(ustore_outcome_t* outcome) {
    return std::atomic_ref<ustore_status_t> {outcome->status}.load(std::memory_order_acquire);
}

char const* ustore_status_name(ustore_status_t status) {
    // A C caller may pass any integer, which C++ may not load as the enum, so its bits are read instead
    std::size_t const index = std::bit_cast<std::underlying_type_t<ustore_status_t>>(status);
    return index < status_names_k.size() ? status_names_k[index].data() : "unrecognized";
}

ustore_status_t ustore_status_from_name(ustore_bytes_view_t name) {
    std::string_view const text {reinterpret_cast<char const*>(name.data), name.length};
    for (std::size_t index = 0; index != status_names_k.size(); ++index)
        if (status_names_k[index] == text) return static_cast<ustore_status_t>(index);
    return ustore_status_invalid_argument_k;
}

char const* ustore_mood_name(ustore_mood_t mood) { return name_in(mood_names_k, mood); }
ustore_mood_t ustore_mood_from_name(ustore_bytes_view_t name) { return value_in(mood_names_k, name); }
char const* ustore_transport_name(ustore_transport_t transport) { return name_in(transport_names_k, transport); }
ustore_transport_t ustore_transport_from_name(ustore_bytes_view_t name) { return value_in(transport_names_k, name); }

void ustore_version(int* major, int* minor, int* patch) {
    *major = USTORE_VERSION_MAJOR;
    *minor = USTORE_VERSION_MINOR;
    *patch = USTORE_VERSION_PATCH;
}

char const* ustore_version_name(void) { return version_name_k.data(); }

void ustore_executor_for_ram(ustore_executor_for_ram_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    if (!arguments->executor)
        return finish(outcome, {status_code_t::invalid_argument_k, "The call needs an output for the executor"});
    *arguments->executor = nullptr;
    st::expected<topology_t> topology = topology_t::make();
    if (!topology) return finish(outcome, pool_failure(topology.status()));
    st::basic_vector<fu_memory_domain_id_t> domains;
    if (status_t const status = read_memory_domains(arguments->ram, *topology, domains); status.failed())
        return finish(outcome, status);
    std::size_t threads = arguments->threads;
    if (!executor_t::spawns_threads()) {
        if (threads > 1) return finish(outcome, {status_code_t::unsupported_k, "This build spawns no threads"});
        threads = 1;
    }
    else if (!threads) threads = std::max<std::size_t>(cores_near(*topology, domains) * 7 / 8, 1);
    // ForkUnion spawns near one memory domain or across every one, so naming several but not all takes the machine
    bool const near_one = domains.size() == 1 && topology->memory_domains_count() > 1;
    st::expected<executor_t> pool = near_one
                                        ? executor_t::make_near(*topology, domains[0], threads, spawned_exclusivity())
                                        : executor_t::make(*topology, threads, spawned_exclusivity());
    if (!pool) return finish(outcome, pool_failure(pool.status()));
    finish(outcome,
           make_executor(std::move(*pool), *topology, domains, arguments->deferred_limit, *arguments->executor));
}

void ustore_executor_on_pool(ustore_executor_on_pool_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    if (!arguments->executor)
        return finish(outcome, {status_code_t::invalid_argument_k, "The call needs an output for the executor"});
    *arguments->executor = nullptr;
    st::expected<lent_pool_t> pool = lent_pool_t::borrow(arguments->pool);
    if (!pool) return finish(outcome, pool_failure(pool.status()));
    st::expected<topology_t> topology = topology_t::make();
    if (!topology) return finish(outcome, pool_failure(topology.status()));
    st::basic_vector<fu_memory_domain_id_t> domains;
    if (status_t const status = read_memory_domains(arguments->ram, *topology, domains); status.failed())
        return finish(outcome, status);
    finish(outcome, make_executor(*pool, *topology, domains, arguments->deferred_limit, *arguments->executor));
}

/** Makes in @p made an executor of @p threads host threads spread over the whole machine, as the NVMe and VRAM
 *  executors take, whose scratch lives near each thread. */
static status_t spawn_across_machine(std::size_t threads, std::size_t deferred_limit,
                                     ustore_executor_t& made) noexcept {
    st::expected<topology_t> topology = topology_t::make();
    if (!topology) return pool_failure(topology.status());
    st::basic_vector<fu_memory_domain_id_t> domains;
    if (status_t const status = read_memory_domains({}, *topology, domains); status.failed()) return status;
    if (!executor_t::spawns_threads()) {
        if (threads > 1) return {status_code_t::unsupported_k, "This build spawns no threads"};
        threads = 1;
    }
    st::expected<executor_t> pool = executor_t::make(*topology, threads, spawned_exclusivity());
    if (!pool) return pool_failure(pool.status());
    return make_executor(std::move(*pool), *topology, domains, deferred_limit, made);
}

void ustore_executor_for_nvme(ustore_executor_for_nvme_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    if (!arguments->executor)
        return finish(outcome, {status_code_t::invalid_argument_k, "The call needs an output for the executor"});
    *arguments->executor = nullptr;
    if (arguments->nvme.transport > ustore_transport_linux_spdk_k)
        return finish(outcome, {status_code_t::invalid_argument_k, "Unknown transport"});
    ustore_executor_handle_t const* const vram = arguments->vram;
    if (vram && vram->residency != residency_t::vram_k)
        return finish(outcome,
                      {status_code_t::device_k, "The compactions need an executor of ustore_executor_for_vram"});
    std::size_t threads = arguments->threads;
    // Every thread pins a channel of 12 MB by default, so the derived count stays as low as the NVMe mount keeps its
    // own
    if (!threads)
        if (st::expected<std::size_t> const cores = executor_t::logical_cores_count(); cores)
            threads = std::clamp<std::size_t>(*cores / 2, 1, 8);
    ustore_executor_t made = nullptr;
    if (status_t const status = spawn_across_machine(std::max<std::size_t>(threads, 1), arguments->deferred_limit,
                                                     made);
        status.failed())
        return finish(outcome, status);
    made->moves_nvme = true;
    made->nvme = arguments->nvme;
    made->vram = vram;
    *arguments->executor = made;
    finish(outcome, {});
}

void ustore_executor_for_vram(ustore_executor_for_vram_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    if (!arguments->executor)
        return finish(outcome, {status_code_t::invalid_argument_k, "The call needs an output for the executor"});
    *arguments->executor = nullptr;
#if USTORE_WITH_CUDA
    st::expected<std::size_t> const visible = cuda_count_devices();
    if (!visible) return finish(outcome, {status_code_t::device_k, "The CUDA runtime counts no devices"});
    if (*visible == 0) return finish(outcome, {status_code_t::device_k, "No CUDA device is visible"});
    bool const derived = !arguments->devices || !arguments->devices_count;
    std::size_t const count = derived ? *visible : static_cast<std::size_t>(arguments->devices_count);
    st::basic_vector<std::int32_t> devices;
    if (st::failed(devices.resize(count)))
        return finish(outcome, {status_code_t::out_of_memory_k, "Failed to copy the devices"});
    for (std::size_t index = 0; index != count; ++index) {
        std::int32_t const ordinal = derived ? static_cast<std::int32_t>(index) : arguments->devices[index];
        if (ordinal < 0 || static_cast<std::size_t>(ordinal) >= *visible)
            return finish(outcome, {status_code_t::device_k, "No CUDA device carries the ordinal"});
        devices[index] = ordinal;
    }
    ustore_executor_t made = nullptr;
    if (status_t const status = spawn_across_machine(count, arguments->deferred_limit, made); status.failed())
        return finish(outcome, status);
    made->residency = residency_t::vram_k;
    made->devices = std::move(devices);
    made->scratch_limit = arguments->scratch_limit ? arguments->scratch_limit : vram_scratch_limit_default_k;
    *arguments->executor = made;
    finish(outcome, {});
#else
    finish(outcome, {status_code_t::unsupported_k, "This build carries no CUDA devices"});
#endif
}

void ustore_executor_drain(ustore_executor_drain_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    ustore_executor_handle_t* const executor = arguments->executor;
    if (arguments->mood != ustore_mood_parked_k && arguments->mood != ustore_mood_spinning_k)
        return finish(outcome, {status_code_t::invalid_argument_k, "The mood is neither parked nor spinning"});
    if (!executor) return finish(outcome, {});
    if (runs_here(*executor))
        return finish(outcome, {status_code_t::contended_k, "A callback cannot drain the executor running it"});
    std::atomic_ref<ustore_mood_t> {executor->mood}.store(arguments->mood, std::memory_order_relaxed);
    wait_finished(*executor,
                  tickets_of(std::atomic_ref<std::uint64_t> {executor->submitted}.load(std::memory_order_acquire)));
    finish(outcome, {});
}

void ustore_executor_free(ustore_executor_t executor) {
    if (!executor) return;
    wait_finished(*executor,
                  tickets_of(std::atomic_ref<std::uint64_t> {executor->submitted}.load(std::memory_order_acquire)));
    if (executor->state == driver_state_t::serving_k) {
        std::atomic_ref<driver_state_t> {executor->state}.store(driver_state_t::stopping_k, std::memory_order_release);
        std::atomic_ref<std::uint32_t> const doorbell {executor->doorbell};
        doorbell.fetch_add(1, std::memory_order_release);
        doorbell.notify_all();
        executor->driver.join(executor->generation);
    }
    for (runner_t& runner : executor->runners) arena_free_blocks(runner.scratch);
    delete executor;
}

void ustore_arena_init(ustore_arena_init_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    if (!arguments->arena)
        return finish(outcome, {status_code_t::invalid_argument_k, "The call needs an output for the arena"});
    *arguments->arena = nullptr;
    if (residency_of(arguments->executor) == residency_t::vram_k)
        return finish(outcome, {status_code_t::unsupported_k, "No engine writes outputs into device memory yet"});
    // Every residency an engine computes in places its arenas on host pages, which the first call allocates
    ustore_arena_t const arena = new (std::nothrow) ustore_arena_handle_t {};
    if (!arena) return finish(outcome, {status_code_t::out_of_memory_k, "Failed to allocate the arena"});
    arena->residency = residency_of(arguments->executor);
    if (arguments->executor) arena->memory_domain = arguments->executor->memory_domain;
    *arguments->arena = arena;
    finish(outcome, {});
}

void ustore_arena_free(ustore_arena_t arena) {
    if (!arena) return;
    wait_deferred_calls(arena->deferred_calls);
    arena_free(arena);
}
