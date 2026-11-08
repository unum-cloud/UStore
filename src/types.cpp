/**
 *  @file src/types.cpp
 *  @author Ash Vardanian
 *  @date September 27, 2026
 *  @brief Implements <tt>c/ustore/types.h</tt>: outcomes, the spelling of statuses and moods, the
 *      version, executors with their drivers, and arenas.
 */

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`
#include <cstring> // `std::memcpy`

#include <algorithm>   // `std::find`, `std::max`, `std::reverse`
#include <array>       // `std::array`
#include <bit>         // `std::bit_ceil`
#include <new>         // `std::nothrow`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <utility>     // `std::move`
#include <variant>     // `std::variant`, `std::get_if`, `std::visit`

#include <smashtable/basic_vector.hpp> // `st::vector`

#include "ustore/types.h" // `ustore_executor_for_ram_t`, `ustore_arena_init_t`, `ustore_version`

#include "lib.hpp" // `ustore_executor_handle_t`, `finish`, `release`, `run_as`, `wait_finished`, `enumerator_spelling`

namespace unum::ustore {

/** The canonical spelling of every status. */
constexpr enumerator_spelling<ustore_status_t> status_names_k[] {
    {ustore_status_success_k, "success"},
    {ustore_status_deferred_k, "deferred"},
    {ustore_status_out_of_memory_k, "out-of-memory"},
    {ustore_status_read_only_k, "read-only"},
    {ustore_status_io_k, "io"},
    {ustore_status_device_k, "device"},
    {ustore_status_malformed_k, "malformed"},
    {ustore_status_internal_k, "internal"},
    {ustore_status_uninitialized_k, "uninitialized"},
    {ustore_status_contended_k, "contended"},
    {ustore_status_cancelled_k, "cancelled"},
    {ustore_status_unsupported_k, "unsupported"},
    {ustore_status_invalid_argument_k, "invalid-argument"},
    {ustore_status_not_found_k, "not-found"},
    {ustore_status_already_exists_k, "already-exists"},
    {ustore_status_write_conflict_k, "write-conflict"},
    {ustore_status_read_conflict_k, "read-conflict"},
    {ustore_status_phantom_conflict_k, "phantom-conflict"},
    {ustore_status_capacity_exhausted_k, "capacity-exhausted"},
};

/** The canonical spelling of every mood. */
constexpr enumerator_spelling<ustore_mood_t> mood_names_k[] {
    {ustore_mood_parked_k, "parked"},
    {ustore_mood_spinning_k, "spinning"},
};

/** The canonical spelling of every GPUDirect Storage mode. */
constexpr enumerator_spelling<ustore_gds_t> gds_names_k[] {
    {ustore_gds_automatic_k, "automatic"},
    {ustore_gds_direct_k, "direct"},
    {ustore_gds_compatibility_k, "compatibility"},
};

/** The canonical spelling of every GPU vendor. */
constexpr enumerator_spelling<ustore_gpu_vendor_t> gpu_vendor_names_k[] {
    {ustore_gpu_vendor_nvidia_k, "nvidia"},
    {ustore_gpu_vendor_amd_k, "amd"},
};

/** The canonical spelling of every GPU class. */
constexpr enumerator_spelling<ustore_gpu_class_t> gpu_class_names_k[] {
    {ustore_gpu_class_nvidia_ampere_k, "nvidia-ampere"},
    {ustore_gpu_class_nvidia_ada_k, "nvidia-ada"},
    {ustore_gpu_class_nvidia_hopper_k, "nvidia-hopper"},
    {ustore_gpu_class_nvidia_blackwell_k, "nvidia-blackwell"},
    {ustore_gpu_class_nvidia_blackwellrtx_k, "nvidia-blackwellrtx"},
    {ustore_gpu_class_amd_cdna3_k, "amd-cdna3"},
    {ustore_gpu_class_amd_cdna4_k, "amd-cdna4"},
    {ustore_gpu_class_amd_cdna5_k, "amd-cdna5"},
};

static_assert(static_cast<std::uint32_t>(gpu_class_t::amd_cdna5_k) == ustore_gpu_class_amd_cdna5_k);
static_assert(static_cast<std::uint32_t>(gpu_vendor_t::amd_k) == ustore_gpu_vendor_amd_k);

/** The canonical spelling of every pattern kind. */
constexpr enumerator_spelling<ustore_pattern_kind_t> pattern_kind_names_k[] {
    {ustore_pattern_regex_k, "regex"},
    {ustore_pattern_substrings_k, "substrings"},
};

/** The canonical spelling of every case sensitivity. */
constexpr enumerator_spelling<ustore_case_sensitivity_t> case_sensitivity_names_k[] {
    {ustore_cased_k, "cased"},
    {ustore_uncased_k, "uncased"},
};

/** The canonical spelling of every predicate cost. */
constexpr enumerator_spelling<ustore_predicate_cost_t> predicate_cost_names_k[] {
    {ustore_predicate_cheap_k, "cheap"},
    {ustore_predicate_expensive_k, "expensive"},
};

/** The canonical spelling of every CPU class, as UCall spells its classes. */
constexpr enumerator_spelling<ustore_cpu_class_t> cpu_class_names_k[] {
    {ustore_cpu_class_unknown_k, "automatic"},
    {ustore_cpu_class_serial_k, "serial"},
    {ustore_cpu_class_intel_haswell_k, "intel-haswell"},
    {ustore_cpu_class_intel_sierra_k, "intel-sierra"},
    {ustore_cpu_class_intel_skylake_k, "intel-skylake"},
    {ustore_cpu_class_intel_icelake_k, "intel-icelake"},
    {ustore_cpu_class_aws_g2_k, "aws-g2"},
    {ustore_cpu_class_apple_m2_k, "apple-m2"},
    {ustore_cpu_class_aws_g5_k, "aws-g5"},
    {ustore_cpu_class_alibaba_c930_k, "alibaba-c930"},
    {ustore_cpu_class_monoversioned_k, "monoversioned"},
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

/** Finishes the outcomes of the first @p count results of @p executor, each right after releasing
 *  the claims its call took and before lowering the deferred counts it raised. No call takes its
 *  arena while another call of the batch still runs, its caller may reuse the arena once the
 *  outcome is final, and a free waiting on it returns after that. */
void publish_results(ustore_executor_handle_t& executor, std::uint32_t count) noexcept {
    for (std::uint32_t task = 0; task != count; ++task) {
        deferred_result_t const& result = executor.results[task];
        release(result.claims);
        finish(*result.outcome, result.status);
        for (std::uint32_t* const deferred_calls : result.deferred_counts) {
            if (!deferred_calls) continue;
            floor_atomic_ref<std::uint32_t> {*deferred_calls}.fetch_sub(1, std::memory_order_release);
        }
    }
}

/** Runs the ready slots from @c head whose tickets follow one another as one batch, answering how many it ran.
 *
 *  The batch's table positions are freed before any of its outcomes is final, so a caller seeing one final may submit
 *  again at once. Nothing reads a slot after that, since a submission may refill it. */
std::uint32_t serve_batch(ustore_executor_handle_t& executor) noexcept {
    std::uint32_t const head = floor_atomic_ref<std::uint32_t> {executor.head}.load(std::memory_order_relaxed);
    std::size_t const mask = executor.deferred.size() - 1;
    deferred_slot_t& first = executor.deferred[head & mask];
    if (floor_atomic_ref<std::uint32_t> {first.ready}.load(std::memory_order_acquire) != head + 1) return 0;
    std::uint32_t const first_ticket = first.ticket;
    std::uint32_t count = 1;
    for (; count != executor.deferred_limit; ++count) {
        deferred_slot_t& slot = executor.deferred[(head + count) & mask];
        if (floor_atomic_ref<std::uint32_t> {slot.ready}.load(std::memory_order_acquire) != head + count + 1 ||
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
    floor_atomic_ref<std::uint32_t> {executor.head}.store(head + count, std::memory_order_release);
    publish_results(executor, count);
    advance_finished(executor, first_ticket + count);
    return count;
}

/** Serves the deferred table of @p executor on its driver thread until @c ustore_executor_free stops it. */
void drive(ustore_executor_handle_t& executor) noexcept {
    floor_atomic_ref<std::uint32_t const> const doorbell {executor.doorbell};
    for (;;) {
        std::uint32_t const rung = doorbell.load(std::memory_order_acquire);
        if (serve_batch(executor) != 0) continue;
        if (floor_atomic_ref<driver_state_t> {executor.state}.load(std::memory_order_acquire) ==
            driver_state_t::stopping_k)
            return;
        if (floor_atomic_ref<ustore_mood_t> {executor.mood}.load(std::memory_order_relaxed) == ustore_mood_spinning_k)
            continue;
        if (executor_t* const owned = std::get_if<executor_t>(&executor.pool)) owned->park(parked_check_micros_k);
        host_verbs().await_change(&executor.doorbell, rung);
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
                             st::vector<fu_memory_domain_id_t>& domains) noexcept {
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
    ustore_nvme_options_t const* const nvme = executor ? std::get_if<ustore_nvme_options_t>(&executor->device)
                                                       : nullptr;
    if (!nvme) return placement;
    placement.nvme = *nvme;
    placement.threads = view_of(executor->pool).threads_count();
    placement.executor = executor;
    return placement;
}

} // namespace unum::ustore

/** Makes in @p made the executor running calls on @p pool, with its driver allowed the ForkUnion capabilities in
 *  @p allowed and a table of @p deferred_limit calls, keeping the scratch of every pool thread on the memory domain
 *  of @p topology local to it when @p domains hold it, and every other scratch and arena on the first of them. */
static status_t make_executor(std::variant<executor_t, lent_pool_t> pool, topology_t const& topology,
                              std::span<fu_memory_domain_id_t const> domains, std::size_t deferred_limit,
                              fu_capabilities_t allowed, ustore_executor_t& made) noexcept {
    std::size_t const limit = deferred_limit ? deferred_limit : deferred_limit_default_k;
    if (limit > deferred_limit_max_k)
        return {status_code_t::capacity_exhausted_k, "An executor holds at most 2^30 deferred calls"};
    st::expected<executor_t> driver = executor_t::make_unpinned(1, spawned_exclusivity(), allowed);
    if (!driver) return pool_failure(driver.status());
    std::size_t const threads = view_of(pool).threads_count();
    ustore_executor_handle_t* const executor = new (std::nothrow)
        ustore_executor_handle_t {std::move(pool), std::move(*driver)};
    if (!executor) return {status_code_t::out_of_memory_k, "Failed to allocate the executor"};
    executor->deferred_limit = static_cast<std::uint32_t>(limit);
    if (st::failed(executor->deferred.resize(std::bit_ceil(limit))) || st::failed(executor->results.resize(limit)) ||
        st::failed(executor->runners.resize(threads + 1))) {
        delete executor;
        return {status_code_t::out_of_memory_k, "Failed to allocate the deferred table"};
    }

    executor->memory_domain = domains.front();
    for (runner_t& runner : executor->runners) runner.scratch.memory = host_blocks_t {.memory_domain = domains.front()};
    // A lent pool running its lender's dispatch refuses this one, leaving every scratch on the first domain
    [[maybe_unused]] st::status_t const placed =
        view_of(executor->pool).for_threads([&](std::size_t thread, std::size_t compute_domain) noexcept {
            fu_memory_domain_id_t const local = topology.memory_domain_near(compute_domain);
            if (serves(domains, local))
                executor->runners[thread].scratch.memory = host_blocks_t {.memory_domain = local};
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

ustore_status_t ustore_outcome_wait(ustore_outcome_t* outcome) { return host_verbs().wait(outcome); }

ustore_status_t ustore_outcome_status(ustore_outcome_t* outcome) { return host_verbs().load(outcome); }

char const* ustore_status_name(ustore_status_t status) { return name_in(status_names_k, status); }

ustore_status_t ustore_status_from_name(ustore_bytes_view_t name) {
    std::string_view const text = string_of(name);
    for (enumerator_spelling<ustore_status_t> const& spelling : status_names_k)
        if (spelling.name == text) return spelling.value;
    return ustore_status_invalid_argument_k;
}

char const* ustore_mood_name(ustore_mood_t mood) { return name_in(mood_names_k, mood); }
ustore_mood_t ustore_mood_from_name(ustore_bytes_view_t name) { return value_in(mood_names_k, name); }
char const* ustore_cpu_class_name(ustore_cpu_class_t cpu_class) { return name_in(cpu_class_names_k, cpu_class); }
ustore_cpu_class_t ustore_cpu_class_from_name(ustore_bytes_view_t name) { return value_in(cpu_class_names_k, name); }
char const* ustore_pattern_kind_name(ustore_pattern_kind_t kind) { return name_in(pattern_kind_names_k, kind); }
ustore_pattern_kind_t ustore_pattern_kind_from_name(ustore_bytes_view_t name) {
    return value_in(pattern_kind_names_k, name);
}
char const* ustore_case_sensitivity_name(ustore_case_sensitivity_t sensitivity) {
    return name_in(case_sensitivity_names_k, sensitivity);
}
ustore_case_sensitivity_t ustore_case_sensitivity_from_name(ustore_bytes_view_t name) {
    return value_in(case_sensitivity_names_k, name);
}
char const* ustore_predicate_cost_name(ustore_predicate_cost_t cost) { return name_in(predicate_cost_names_k, cost); }
ustore_predicate_cost_t ustore_predicate_cost_from_name(ustore_bytes_view_t name) {
    return value_in(predicate_cost_names_k, name);
}
char const* ustore_gds_name(ustore_gds_t gds) { return name_in(gds_names_k, gds); }
ustore_gds_t ustore_gds_from_name(ustore_bytes_view_t name) { return value_in(gds_names_k, name); }
char const* ustore_gpu_vendor_name(ustore_gpu_vendor_t vendor) { return name_in(gpu_vendor_names_k, vendor); }
ustore_gpu_vendor_t ustore_gpu_vendor_from_name(ustore_bytes_view_t name) { return value_in(gpu_vendor_names_k, name); }
char const* ustore_gpu_class_name(ustore_gpu_class_t gpu_class) { return name_in(gpu_class_names_k, gpu_class); }
ustore_gpu_class_t ustore_gpu_class_from_name(ustore_bytes_view_t name) { return value_in(gpu_class_names_k, name); }
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
    auto const [ceiling, pick_status] = pick_cpu_ceiling(arguments->cpu_class);
    if (pick_status.failed()) return finish(outcome, pick_status);
    fu_capabilities_t const allowed = forkunion_allowed_of(ceiling);
    st::expected<topology_t> topology = topology_t::make();
    if (!topology) return finish(outcome, pool_failure(topology.status()));
    st::vector<fu_memory_domain_id_t> domains;
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
    st::expected<executor_t> pool = near_one ? executor_t::make_near(*topology, domains[0], threads,
                                                                     spawned_exclusivity(), allowed)
                                             : executor_t::make(*topology, threads, spawned_exclusivity(), allowed);
    if (!pool) return finish(outcome, pool_failure(pool.status()));
    finish(outcome, make_executor(std::move(*pool), *topology, domains, arguments->deferred_limit, allowed,
                                  *arguments->executor));
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
    st::vector<fu_memory_domain_id_t> domains;
    if (status_t const status = read_memory_domains(arguments->ram, *topology, domains); status.failed())
        return finish(outcome, status);
    finish(outcome, make_executor(*pool, *topology, domains, arguments->deferred_limit, fu_capabilities_all_k,
                                  *arguments->executor));
}

/** Makes in @p made an executor of @p threads host threads spread over the whole machine, as the NVMe and VRAM
 *  executors take, allowed the ForkUnion capabilities in @p allowed, whose scratch lives near each thread. */
static status_t spawn_across_machine(std::size_t threads, std::size_t deferred_limit, fu_capabilities_t allowed,
                                     ustore_executor_t& made) noexcept {
    st::expected<topology_t> topology = topology_t::make();
    if (!topology) return pool_failure(topology.status());
    st::vector<fu_memory_domain_id_t> domains;
    if (status_t const status = read_memory_domains({}, *topology, domains); status.failed()) return status;
    if (!executor_t::spawns_threads()) {
        if (threads > 1) return {status_code_t::unsupported_k, "This build spawns no threads"};
        threads = 1;
    }
    st::expected<executor_t> pool = executor_t::make(*topology, threads, spawned_exclusivity(), allowed);
    if (!pool) return pool_failure(pool.status());
    return make_executor(std::move(*pool), *topology, domains, deferred_limit, allowed, made);
}

void ustore_executor_for_nvme(ustore_executor_for_nvme_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    if (!arguments->executor)
        return finish(outcome, {status_code_t::invalid_argument_k, "The call needs an output for the executor"});
    *arguments->executor = nullptr;
    if (arguments->nvme.transport > ustore_transport_linux_spdk_k)
        return finish(outcome, {status_code_t::invalid_argument_k, "Unknown transport"});
    auto const [ceiling, pick_status] = pick_cpu_ceiling(arguments->cpu_class);
    if (pick_status.failed()) return finish(outcome, pick_status);
    std::size_t threads = arguments->threads;
    // Every thread pins a channel of 12 MB by default, so the derived count stays as low as the NVMe mount keeps its
    // own
    if (!threads)
        if (st::expected<std::size_t> const cores = executor_t::logical_cores_count(); cores)
            threads = std::clamp<std::size_t>(*cores / 2, 1, 8);
    ustore_executor_t made = nullptr;
    if (status_t const status = spawn_across_machine(std::max<std::size_t>(threads, 1), arguments->deferred_limit,
                                                     forkunion_allowed_of(ceiling), made);
        status.failed())
        return finish(outcome, status);
    made->device = arguments->nvme;
    *arguments->executor = made;
    finish(outcome, {});
}

void ustore_executor_for_vram(ustore_executor_for_vram_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    if (!arguments->executor)
        return finish(outcome, {status_code_t::invalid_argument_k, "The call needs an output for the executor"});
    *arguments->executor = nullptr;
    if (arguments->gds > ustore_gds_compatibility_k)
        return finish(outcome, {status_code_t::invalid_argument_k, "Unknown GPUDirect Storage mode"});
    std::span<gpu_device_t const> const devices = gpu_devices();
    if (arguments->device < 0 || static_cast<std::size_t>(arguments->device) >= devices.size())
        return finish(outcome, {status_code_t::device_k, "No GPU carries the ordinal"});
    gpu_device_t const& device = devices[static_cast<std::size_t>(arguments->device)];
    if (device.vendor != gpu_vendor_t::nvidia_k && arguments->gds == ustore_gds_direct_k)
        return finish(outcome, {status_code_t::device_k, "GPUDirect Storage reaches NVIDIA devices alone"});
    // One host thread keeps the device fed, as every call only queues work on its stream
    ustore_executor_t made = nullptr;
    if (status_t const status = spawn_across_machine(1, arguments->deferred_limit, fu_capabilities_all_k, made);
        status.failed())
        return finish(outcome, status);
    gpu_runtime_t& runtime = gpu_runtime();
    if (status_t const status = runtime.spawn(device, gpu_class_of(device), arguments->stream, arguments->scratch_limit,
                                              arguments->gds, made->device);
        status.failed()) {
        ustore_executor_free(made);
        return finish(outcome, status);
    }
    *arguments->executor = made;
    finish(outcome, {});
}

void ustore_gpu_devices(ustore_gpu_devices_t* arguments) {
    std::span<gpu_device_t const> const devices = gpu_devices();
    std::size_t const copied = arguments->devices ? std::min<std::size_t>(devices.size(), arguments->devices_capacity)
                                                  : 0;
    for (std::size_t position = 0; position != copied; ++position) {
        gpu_device_t const& device = devices[position];
        ustore_gpu_device_t& listed = arguments->devices[position];
        listed = {static_cast<ustore_gpu_vendor_t>(device.vendor),
                  device.vendor_ordinal,
                  static_cast<ustore_gpu_class_t>(gpu_class_of(device)),
                  device.compute_units,
                  device.memory_bytes,
                  {}};
        std::memcpy(listed.name, device.name, sizeof(listed.name));
    }
    if (arguments->devices_count) *arguments->devices_count = devices.size();
    finish(*arguments->outcome, {});
}

void ustore_executor_drain(ustore_executor_drain_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    ustore_executor_handle_t* const executor = arguments->executor;
    if (arguments->mood != ustore_mood_parked_k && arguments->mood != ustore_mood_spinning_k)
        return finish(outcome, {status_code_t::invalid_argument_k, "The mood is neither parked nor spinning"});
    if (!executor) return finish(outcome, {});
    if (runs_here(*executor))
        return finish(outcome, {status_code_t::contended_k, "A callback cannot drain the executor running it"});
    floor_atomic_ref<ustore_mood_t> {executor->mood}.store(arguments->mood, std::memory_order_relaxed);
    wait_finished(*executor,
                  tickets_of(floor_atomic_ref<std::uint64_t> {executor->submitted}.load(std::memory_order_acquire)));
    finish(outcome, {});
}

void ustore_executor_free(ustore_executor_t executor) {
    if (!executor) return;
    wait_finished(*executor,
                  tickets_of(floor_atomic_ref<std::uint64_t> {executor->submitted}.load(std::memory_order_acquire)));
    if (executor->state == driver_state_t::serving_k) {
        floor_atomic_ref<driver_state_t> {executor->state}.store(driver_state_t::stopping_k, std::memory_order_release);
        floor_atomic_ref<std::uint32_t> {executor->doorbell}.fetch_add(1, std::memory_order_release);
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
    ustore_arena_t const arena = new (std::nothrow) ustore_arena_handle_t {};
    if (!arena) return finish(outcome, {status_code_t::out_of_memory_k, "Failed to allocate the arena"});
    // Nothing is allocated until the first call writes, on the memory domain or the device its executor names
    if (ustore_executor_t const executor = arguments->executor)
        std::visit(
            [&]<typename device_type_>(device_type_ const& device) noexcept {
                if constexpr (gpu_executor_concept<device_type_>)
                    arena->memory = typename device_type_::arena_blocks_t {.device = device.device()};
                else arena->memory = host_blocks_t {.memory_domain = executor->memory_domain};
            },
            executor->device);
    *arguments->arena = arena;
    finish(outcome, {});
}

void ustore_arena_free(ustore_arena_t arena) {
    if (!arena) return;
    wait_deferred_calls(arena->deferred_calls);
    destroy_arena(arena);
}

void ustore_arena_describe(ustore_arena_describe_t* arguments) {
    ustore_outcome_t& outcome = *arguments->outcome;
    ustore_arena_t const arena = arguments->arena;
    if (!arena || !arguments->description)
        return finish(outcome, {status_code_t::invalid_argument_k, "The call needs an arena and a description output"});
    ustore_arena_description_t& description = *arguments->description;
    description = {ustore_residency_ram_k, -1};
    gpu_vendor_t vendor = gpu_vendor_t::nvidia_k;
    std::int32_t vendor_ordinal = -1;
    if (cuda_blocks_t const* const cuda = std::get_if<cuda_blocks_t>(&arena->memory)) vendor_ordinal = cuda->device;
    if (hip_blocks_t const* const hip = std::get_if<hip_blocks_t>(&arena->memory)) {
        vendor = gpu_vendor_t::amd_k;
        vendor_ordinal = hip->device;
    }
    if (vendor_ordinal < 0) return finish(outcome, {});
    description.residency = ustore_residency_vram_k;
    std::span<gpu_device_t const> const devices = gpu_devices();
    for (std::size_t position = 0; position != devices.size(); ++position)
        if (devices[position].vendor == vendor &&
            devices[position].vendor_ordinal == static_cast<std::uint32_t>(vendor_ordinal))
            description.device = static_cast<std::int32_t>(position);
    finish(outcome, {});
}
