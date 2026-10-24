/**
 *  @file test/nvme/write_buffer.cpp
 *  @author Ash Vardanian
 *  @date July 10, 2023
 *  @brief Tests adding, removing, merging, copying and moving stored values in the write buffer.
 */
#undef NDEBUG // ! A test's oracle must stay live in every build

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`
#include <cstring> // `std::memcmp`

#include <array>   // `std::array`
#include <limits>  // `std::numeric_limits`
#include <memory>  // `std::make_shared`
#include <utility> // `std::move`

#include <fmt/format.h>

#include <forkunion/atomics.hpp> // `fu::standard_atomic_ref`
#include <forkunion/types.hpp>   // `fu::standard_yield_t`
#include <smashtable/shared.hpp> // `st::standard_waiting_policy`

#include "ustore/blobs/nvme_write_buffer.hpp" // `write_buffer`, `write_buffer_status_t`

#include "harness.hpp" // `suite_exit_code`, `expect`, `expect_eq`

using namespace unum::ustore;
using namespace unum::ustore::test;
using namespace unum::ustore::blobs;
using namespace unum::ustore::utilities;

/** The keys every case writes. */
using stored_key_t = std::size_t;

/** The atomic reference the buffer is instantiated with: the portable one. */
template <typename value_type_>
using atomic_reference = fu::standard_atomic_ref<value_type_>;

/** The waiting policy the buffer is instantiated with, yielding between polls. */
using waiting_policy_t = st::standard_waiting_policy<fu::standard_yield_t>;

using resources_t = shared_resources<atomic_reference, waiting_policy_t>;
using resources_shared_ptr_t = resources_shared_ptr<atomic_reference, waiting_policy_t>;
using countdown_t = countdown<atomic_reference, waiting_policy_t>;

/** The write buffer every case fills. */
using write_buffer_t = write_buffer<stored_key_t, atomic_reference, waiting_policy_t>;

/** Flags of this suite, with their defaults. */
struct options_t {

    /** Stored values every case writes into the cache. */
    count_t stored_values {1000};

    /** Every this many stored values is empty. */
    count_t empty_value_step {50};

    /** Largest stored value the cache admits. */
    bytes_t value_max_size {10000};
};

/** Bytes every stored value of the suite takes together, which sizes each cache. */
std::size_t stored_values_size_sum = 0;

/** The budgets every cache charges, checked to be returned after every case. */
resources_shared_ptr_t resources;

/** The keys every case writes, one per stored value. */
st::basic_vector<stored_key_t> stored_keys;

/** The stored values every case writes, random bytes of random lengths, empty every @c empty_value_step. */
st::basic_vector<buffer_t> stored_values;

/** Fills the keys and the stored values every case shares, answering whether they fit in memory. */
bool prepare_initial_data(options_t const& options, random_t& random) {
    std::size_t const count = options.stored_values.value;
    if (stored_keys.resize(count) != st::status_t::success_k) return false;
    if (stored_values.resize(count) != st::status_t::success_k) return false;
    for (std::size_t index = 0; index != count; ++index) {
        stored_keys[index] = index;
        if (index % options.empty_value_step.value == options.empty_value_step.value - 1) continue;
        std::size_t const length = 1 + random.below(options.value_max_size.value);
        if (stored_values[index].resize(length) != st::status_t::success_k) return false;
        for (std::byte& byte : stored_values[index]) byte = static_cast<std::byte>(random.next());
        stored_values_size_sum += length;
    }
    return true;
}

/** Whether every byte any cache took from the budgets went back. */
std::size_t ensure_all_resources_are_freed() {
    std::size_t failures = 0;
    failures += expect_eq(resources->memory_constraint.used(), std::size_t {0});
    failures += expect_eq(resources->gpu_memory_constraint.used(), std::size_t {0});
    return failures;
}

/** Whether @p cache holds nothing and answers every key as missing. */
std::size_t ensure_cache_is_empty(write_buffer_t const& cache) {
    std::size_t failures = 0;
    failures += expect_eq(cache.count(), std::size_t {0});
    for (stored_key_t const stored_key : stored_keys)
        failures += expect(cache.status(stored_key) == write_buffer_status_t::mismatch_k);
    return failures;
}

/** Whether every key of @p cache answers as @p status_must_be says, with the bytes it was written with. */
template <typename status_must_be_>
std::size_t check_stored_values(options_t const& options, st::basic_vector<buffer_t> const& values,
                                write_buffer_t const& cache, status_must_be_ const& status_must_be) {

    std::size_t failures = 0;
    using stored_value_location_t = stored_value_location<value_layout_t::variable_width_k>;

    for (std::size_t index = 0; index < options.stored_values.value; ++index) {
        write_buffer_status_t const status = cache.status(stored_keys[index]);
        failures += expect(status == status_must_be(index));
        stored_value_location_t location;
        write_buffer_status_t const found = cache.find(stored_keys[index], location);
        failures += expect(found == status);
        if (status != write_buffer_status_t::found_k) continue;
        failures += expect(static_cast<bool>(location));
        if (location.size() == 0) continue;

        aligned_buffer_t stored_value;
        failures += expect(stored_value.resize(location.aligned_buffer_size()) == st::status_t::success_k);
        if (stored_value.size() != location.aligned_buffer_size()) continue;
        countdown_t countdown {1};
        cache.select(location, as_bytes_span(stored_value), countdown);
        failures += expect(countdown.wait());
        failures += expect(std::memcmp(stored_value.data() + location.buffer_padding(), values[index].data(),
                                       values[index].size()) == 0);
    }
    return failures;
}

std::size_t test_initial_state(options_t const& options) {

    std::size_t failures = 0;
    write_buffer_t cache(stored_values_size_sum, options.stored_values.value, 1, 0, options.value_max_size.value,
                         resources);
    failures += ensure_cache_is_empty(cache);
    cache.unsafe_clear();
    failures += ensure_cache_is_empty(cache);
    return failures;
}

std::size_t test_insertions(options_t const& options, st::basic_vector<buffer_t> const& stored_values) {

    std::size_t failures = 0;
    write_buffer_t cache(2 * stored_values_size_sum, options.stored_values.value, 1, 0, options.value_max_size.value,
                         resources);

    for (std::size_t index = 0; index < options.stored_values.value; ++index) {
        auto status = cache.add(stored_keys[index], as_bytes_view(stored_values[index]));
        failures += expect(status.succeeded());
    }

    auto filter = [](std::size_t) { return write_buffer_status_t::found_k; };
    failures += check_stored_values(options, stored_values, cache, filter);

    cache.unsafe_clear();
    failures += ensure_cache_is_empty(cache);
    return failures;
}

std::size_t test_removals(options_t const& options, st::basic_vector<buffer_t> const& stored_values) {

    std::size_t failures = 0;
    write_buffer_t cache(2 * stored_values_size_sum, options.stored_values.value, 1, 0, options.value_max_size.value,
                         resources);

    for (std::size_t index = 0; index < options.stored_values.value; ++index) {
        auto status = cache.mark_as_removed(stored_keys[index]);
        failures += expect(status.succeeded());
    }

    auto filter = [](std::size_t) { return write_buffer_status_t::removed_k; };
    failures += check_stored_values(options, stored_values, cache, filter);

    cache.unsafe_clear();
    failures += ensure_cache_is_empty(cache);
    return failures;
}

std::size_t test_mixed_insertions_and_removals(options_t const& options,
                                               st::basic_vector<buffer_t> const& stored_values) {

    std::size_t failures = 0;
    write_buffer_t cache(2 * stored_values_size_sum, options.stored_values.value, 1, 0, options.value_max_size.value,
                         resources);

    for (std::size_t index = 0; index < options.stored_values.value / 2; ++index) {
        if (index % 2 == 0) {
            auto status = cache.add(stored_keys[index], as_bytes_view(stored_values[index]));
            failures += expect(status.succeeded());
        }
        else {
            auto status = cache.mark_as_removed(stored_keys[index]);
            failures += expect(status.succeeded());
        }
    }
    for (std::size_t index = options.stored_values.value / 2; index < options.stored_values.value; ++index) {
        if (index % 2 == 0) {
            auto status = cache.mark_as_removed(stored_keys[index]);
            failures += expect(status.succeeded());
        }
        else {
            auto status = cache.add(stored_keys[index], as_bytes_view(stored_values[index]));
            failures += expect(status.succeeded());
        }
    }

    auto filter = [&options](std::size_t index) {
        if (index < options.stored_values.value / 2 && index % 2 == 0) return write_buffer_status_t::found_k;
        else if (index >= options.stored_values.value / 2 && index % 2 != 0) return write_buffer_status_t::found_k;
        else return write_buffer_status_t::removed_k;
    };
    failures += check_stored_values(options, stored_values, cache, filter);

    cache.unsafe_clear();
    failures += ensure_cache_is_empty(cache);
    return failures;
}

std::size_t test_overload(options_t const& options) {

    std::size_t failures = 0;
    write_buffer_t cache(100, options.stored_values.value - 1, 1, 0, options.value_max_size.value, resources);

    status_t status = status_t {};
    for (std::size_t index = 0; index < options.stored_values.value; ++index) {
        status_t const current_status = cache.mark_as_removed(stored_keys[index]);
        if (current_status.code != status.code) {
            failures += expect(status.succeeded());
            failures += expect_status(current_status, status_code_t::capacity_exhausted_k);
            status = status_t {status_code_t::capacity_exhausted_k, "Cache is full"};
        }
    }
    failures += expect_status(status, status_code_t::capacity_exhausted_k);

    cache.unsafe_clear();
    failures += ensure_cache_is_empty(cache);
    return failures;
}

std::size_t test_batch(options_t const& options, st::basic_vector<buffer_t> const& stored_values) {

    std::size_t failures = 0;
    write_buffer_t first_cache(4 * stored_values_size_sum, 2 * options.stored_values.value, 1, 0,
                               options.value_max_size.value, resources);
    for (std::size_t index = 0; index < options.stored_values.value / 2; ++index) {
        auto status = first_cache.add(stored_keys[index], as_bytes_view(stored_values[index]));
        failures += expect(status.succeeded());
        if (index % 2 == 1) {
            status = first_cache.mark_as_removed(stored_keys[index]);
            failures += expect(status.succeeded());
        }
    }

    write_buffer_t second_cache(2 * stored_values_size_sum, options.stored_values.value, 1, 0,
                                options.value_max_size.value, resources);
    for (std::size_t index = options.stored_values.value / 2; index < options.stored_values.value; ++index) {
        auto status = second_cache.add(stored_keys[index], as_bytes_view(stored_values[index]));
        failures += expect(status.succeeded());
        if (index % 2 == 1) {
            status = second_cache.mark_as_removed(stored_keys[index]);
            failures += expect(status.succeeded());
        }
    }

    failures += expect(first_cache.can_fit(second_cache.stored_values.size(), second_cache.next_stored_value_offset_));
    failures += expect(first_cache.unsafe_add(second_cache).succeeded());

    auto filter = [](std::size_t index) {
        if (index % 2 == 0) return write_buffer_status_t::found_k;
        else return write_buffer_status_t::removed_k;
    };
    failures += check_stored_values(options, stored_values, first_cache, filter);

    first_cache.unsafe_clear();
    second_cache.unsafe_clear();
    failures += ensure_cache_is_empty(first_cache);
    failures += ensure_cache_is_empty(second_cache);
    return failures;
}

std::size_t test_batch_overload(options_t const& options, st::basic_vector<buffer_t> const& stored_values) {

    std::size_t failures = 0;
    write_buffer_t first_cache(stored_values_size_sum / 4, options.stored_values.value, 1, 0,
                               options.value_max_size.value, resources);
    auto status = first_cache.add(stored_keys[0], as_bytes_view(stored_values[0]));
    failures += expect(status.succeeded());

    write_buffer_t second_cache(2 * stored_values_size_sum, options.stored_values.value, 1, 0,
                                options.value_max_size.value, resources);
    for (std::size_t index = options.stored_values.value / 2; index < options.stored_values.value; ++index) {
        status = second_cache.add(stored_keys[index], as_bytes_view(stored_values[index]));
        failures += expect(status.succeeded());
        if (index % 2 == 1) {
            status = second_cache.mark_as_removed(stored_keys[index]);
            failures += expect(status.succeeded());
        }
    }

    failures += expect(!first_cache.can_fit(second_cache.stored_values.size(), second_cache.next_stored_value_offset_));

    first_cache.unsafe_clear();
    second_cache.unsafe_clear();
    failures += ensure_cache_is_empty(first_cache);
    failures += ensure_cache_is_empty(second_cache);
    return failures;
}

std::size_t test_copy(options_t const& options, st::basic_vector<buffer_t> const& stored_values) {

    std::size_t failures = 0;
    write_buffer_t cache(2 * stored_values_size_sum, options.stored_values.value, 1, 0, options.value_max_size.value,
                         resources);

    for (std::size_t index = 0; index < options.stored_values.value / 2; ++index) {
        auto status = cache.add(stored_keys[index], as_bytes_view(stored_values[index]));
        failures += expect(status.succeeded());
    }

    write_buffer_t cache_copy(cache);
    for (std::size_t index = options.stored_values.value / 2; index < options.stored_values.value; ++index) {
        auto status = cache.add(stored_keys[index], as_bytes_view(stored_values[index]));
        failures += expect(status.succeeded());
    }

    auto filter1 = [&options](std::size_t index) {
        return (index < options.stored_values.value / 2) ? write_buffer_status_t::found_k
                                                         : write_buffer_status_t::mismatch_k;
    };
    failures += check_stored_values(options, stored_values, cache_copy, filter1);

    cache_copy = cache;
    auto filter2 = [](std::size_t) { return write_buffer_status_t::found_k; };
    failures += check_stored_values(options, stored_values, cache_copy, filter2);

    cache.unsafe_clear();
    cache_copy.unsafe_clear();
    failures += ensure_cache_is_empty(cache);
    failures += ensure_cache_is_empty(cache_copy);
    return failures;
}

std::size_t test_move(options_t const& options, st::basic_vector<buffer_t> const& stored_values) {

    std::size_t failures = 0;
    write_buffer_t cache(2 * stored_values_size_sum, options.stored_values.value, 1, 0, options.value_max_size.value,
                         resources);

    for (std::size_t index = 0; index < options.stored_values.value / 2; ++index) {
        auto status = cache.add(stored_keys[index], as_bytes_view(stored_values[index]));
        failures += expect(status.succeeded());
    }

    write_buffer_t cache_moved(std::move(cache));
    for (std::size_t index = options.stored_values.value / 2; index < options.stored_values.value; ++index) {
        auto status = cache_moved.add(stored_keys[index], as_bytes_view(stored_values[index]));
        failures += expect(status.succeeded());
    }

    auto filter = [](std::size_t) { return write_buffer_status_t::found_k; };
    failures += check_stored_values(options, stored_values, cache_moved, filter);

    cache = std::move(cache_moved);
    cache_moved = std::move(cache);
    failures += check_stored_values(options, stored_values, cache_moved, filter);

    cache_moved.unsafe_clear();
    failures += ensure_cache_is_empty(cache_moved);
    return failures;
}

std::size_t cache_initial_state(options_t const& options) {
    std::size_t failures = 0;
    failures += test_initial_state(options);
    failures += ensure_all_resources_are_freed();
    return failures;
}

std::size_t cache_insertions(options_t const& options) {
    std::size_t failures = 0;
    failures += test_insertions(options, stored_values);
    failures += ensure_all_resources_are_freed();
    return failures;
}

std::size_t cache_removals(options_t const& options) {
    std::size_t failures = 0;
    failures += test_removals(options, stored_values);
    failures += ensure_all_resources_are_freed();
    return failures;
}

std::size_t cache_mixed_insertions_and_removals(options_t const& options) {
    std::size_t failures = 0;
    failures += test_mixed_insertions_and_removals(options, stored_values);
    failures += ensure_all_resources_are_freed();
    return failures;
}

std::size_t cache_test_overload(options_t const& options) {
    std::size_t failures = 0;
    failures += test_overload(options);
    failures += ensure_all_resources_are_freed();
    return failures;
}

std::size_t cache_test_batch(options_t const& options) {
    std::size_t failures = 0;
    failures += test_batch(options, stored_values);
    failures += ensure_all_resources_are_freed();
    return failures;
}

std::size_t cache_test_batch_overload(options_t const& options) {
    std::size_t failures = 0;
    failures += test_batch_overload(options, stored_values);
    failures += ensure_all_resources_are_freed();
    return failures;
}

std::size_t cache_copy(options_t const& options) {
    std::size_t failures = 0;
    failures += test_copy(options, stored_values);
    failures += ensure_all_resources_are_freed();
    return failures;
}

std::size_t cache_move(options_t const& options) {
    std::size_t failures = 0;
    failures += test_move(options, stored_values);
    failures += ensure_all_resources_are_freed();
    return failures;
}

int main(int argc, char** argv) {
    options_t options;
    std::array<flag_t, 3> const flags {{
        {"--stored-values", "count", "stored values every case writes into the cache", &options.stored_values},
        {"--empty-value-step", "count", "every this many stored values is empty", &options.empty_value_step},
        {"--value-max-size", "size", "largest stored value the cache admits, like 10KB", &options.value_max_size},
    }};
    suite_options_t suite;
    if (argument_status_t const status = parse_suite_options(argc, argv, suite, flags);
        status != argument_status_t::parsed_k)
        return status == argument_status_t::help_k ? 0 : 1;
    suite_totals_t totals = suite.totals();
    if (options.value_max_size.value == 0 ||
        options.value_max_size.value > std::numeric_limits<stored_value_size_t>::max()) {
        fmt::println(stderr, "--value-max-size: must be positive and fit a stored value size");
        return 1;
    }

    resources = std::make_shared<resources_t>(40'000'000, 0);
    random_t random = suite.random();
    if (!prepare_initial_data(options, random)) {
        fmt::println(stderr, "the stored values of the suite did not fit in memory");
        return 1;
    }

    report(totals, "cache_initial_state", cache_initial_state, options);
    report(totals, "cache_insertions", cache_insertions, options);
    report(totals, "cache_removals", cache_removals, options);
    report(totals, "cache_mixed_insertions_and_removals", cache_mixed_insertions_and_removals, options);
    report(totals, "cache_test_overload", cache_test_overload, options);
    report(totals, "cache_test_batch", cache_test_batch, options);
    report(totals, "cache_test_batch_overload", cache_test_batch_overload, options);
    report(totals, "cache_copy", cache_copy, options);
    report(totals, "cache_move", cache_move, options);

    return suite_exit_code(totals);
}
