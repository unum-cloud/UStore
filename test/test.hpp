/**
 *  @file test/test.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief Shared checks for the C++ test suites: failure counting, results, the totals, bounded waits, flags and
 *      directories.
 *
 *  Every check returns its own failure count, a case returns a @c case_result_t, and @c report runs each case the
 *  @c --filter selects and rolls its result into the @c suite_totals_t the suite's @c main owns, so counts reach
 *  @c suite_exit_code without global mutable state.
 */
#pragma once

#include <cerrno>  // `errno`, `EEXIST`
#include <cmath>   // `std::fabs`, `std::fmax`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstdio>  // `stderr`, `std::remove`
#include <cstdlib> // `mkdtemp`
#include <cstring> // `std::memcpy`, `std::strlen`

#include <ftw.h>      // `nftw`
#include <sys/stat.h> // `mkdir`

#include <array>       // `std::array`
#include <optional>    // `std::optional`
#include <random>      // `std::random_device`
#include <regex>       // `std::regex`, `std::regex_search`
#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <thread>      // `std::thread::hardware_concurrency`
#include <type_traits> // `std::type_identity_t`

#include <fmt/format.h>

#include <forkunion/capabilities.hpp> // `fu::preferred_yield_t`
#include <smashtable/shared.hpp>

#include <ustore/db.h>
#include <ustore/status.hpp> // `status_t`, `status_code_t`, `expected`
#include <ustore/types.hpp>

#include "arguments.hpp"

namespace unum::ustore::test {

/** Counts one failure when the condition is false, printing it with its location. */
#define expect(...) expect_((bool)(__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
inline std::size_t expect_(bool condition, char const* expression, char const* file, int line) {
    if (!condition) fmt::println(stderr, "  check failed at {}:{}: {}", file, line, expression);
    return condition ? 0u : 1u;
}

/** Counts one failure when two values differ, printing both. */
#define expect_eq(actual, wanted) expect_eq_((actual), (wanted), #actual " == " #wanted, __FILE__, __LINE__)
template <typename actual_type_, typename wanted_type_>
inline std::size_t expect_eq_(actual_type_ const& actual, wanted_type_ const& wanted, char const* expression,
                              char const* file, int line) {
    if (actual == wanted) return 0;
    fmt::println(stderr, "  check failed at {}:{}: {} - expected {}, got {}", file, line, expression, wanted, actual);
    return 1;
}

/** Counts one failure when two floats differ past a 1e-3 relative tolerance, printing both. */
#define expect_near(actual, wanted) expect_near_((actual), (wanted), #actual " ~ " #wanted, __FILE__, __LINE__)
inline std::size_t expect_near_(double actual, double wanted, char const* expression, char const* file, int line) {
    double const scale = std::fmax(std::fmax(std::fabs(actual), std::fabs(wanted)), 1.0);
    double const difference = std::fabs(actual - wanted);
    if (difference <= 1e-3 * scale) return 0;
    fmt::println(stderr, "  check failed at {}:{}: {} - expected {:.6g}, got {:.6g}, off by {:.4g}", file, line,
                 expression, wanted, actual, difference);
    return 1;
}

/** The enumerator's own spelling of @p code, so a message names what the code says. */
[[nodiscard]] constexpr char const* describe(status_code_t code) noexcept {
    switch (code) {
    case status_code_t::success_k: return "success_k";
    case status_code_t::out_of_memory_k: return "out_of_memory_k";
    case status_code_t::invalid_argument_k: return "invalid_argument_k";
    case status_code_t::uninitialized_k: return "uninitialized_k";
    case status_code_t::unsupported_k: return "unsupported_k";
    case status_code_t::input_output_k: return "input_output_k";
    case status_code_t::malformed_k: return "malformed_k";
    case status_code_t::not_found_k: return "not_found_k";
    case status_code_t::already_exists_k: return "already_exists_k";
    case status_code_t::write_conflict_k: return "write_conflict_k";
    case status_code_t::read_conflict_k: return "read_conflict_k";
    case status_code_t::phantom_conflict_k: return "phantom_conflict_k";
    case status_code_t::capacity_exhausted_k: return "capacity_exhausted_k";
    case status_code_t::device_k: return "device_k";
    case status_code_t::internal_k: return "internal_k";
    }
    return "unknown";
}

/** Counts one failure when @p actual carries a code other than @p wanted, printing both with the message. */
#define expect_status(actual, wanted) expect_status_((actual), (wanted), __FILE__, __LINE__)
inline std::size_t expect_status_(status_t actual, status_code_t wanted, char const* file, int line) {
    if (actual.code == wanted) return 0;
    fmt::println(stderr, "  check failed at {}:{}: expected {}, got {}{}{}", file, line, describe(wanted),
                 describe(actual.code), actual.message ? ": " : "", actual.message ? actual.message : "");
    return 1;
}

/** The same check on the status an @c expected carries, so a refusal is checked without unpacking it. */
template <typename value_type_>
inline std::size_t expect_status_(expected<value_type_> const& actual, status_code_t wanted, char const* file,
                                  int line) {
    return expect_status_(actual.status(), wanted, file, line);
}

/** What one case answers: its failures, and the reason it could not run when it skipped. */
struct case_result_t {

    /** Checks that failed, which a case skipping past its gate still carries from before it. */
    std::size_t failures = 0;

    /** Why the case did not run, or null when it ran. */
    char const* skip_reason = nullptr;

    /** A result of @p counted failures, so a case answering its count reads as one. */
    constexpr case_result_t(std::size_t counted) noexcept : failures(counted) {}

    /** A result that did not run for @p reason, carrying the @p counted failures from before its gate. */
    [[nodiscard]] static constexpr case_result_t skipped(char const* reason, std::size_t counted = 0) noexcept {
        case_result_t result {counted};
        result.skip_reason = reason;
        return result;
    }
};

/** Whether a skipped case is a tolerated absence of hardware or option, or a refused gap in coverage. */
enum class skip_policy_t : std::uint8_t {
    tolerated_k,
    refused_k,
};

/** The counts one suite's @c main owns and every @c report rolls into, so nothing global accumulates. */
struct suite_totals_t {

    /** Cases that ran without a failure. */
    std::size_t passed = 0;

    /** Cases that did not run. */
    std::size_t skipped = 0;

    /** Cases with at least one failure. */
    std::size_t failed = 0;

    /** Failed checks across every case. */
    std::size_t failures = 0;

    /** The seed every random draw of the suite descends from. */
    std::uint64_t seed = 0;

    /** What a skip does to the exit code. */
    skip_policy_t policy = skip_policy_t::tolerated_k;

    /** The @c --filter pattern a case label must match to run, empty to run every case. */
    std::string_view filter;

    /** The filter compiled once as an ECMAScript regex, or nothing when it does not compile. */
    std::optional<std::regex> pattern;

    /** The binary's @c argv[0], which turns a rerun line into a command. */
    char const* program = "";

    /** Whether the case labelled @p name runs: the filter searched as a regex, or found as a plain substring. */
    [[nodiscard]] bool selects(std::string_view name) const noexcept {
        if (filter.empty()) return true;
        if (pattern) return std::regex_search(name.begin(), name.end(), *pattern);
        return name.find(filter) != std::string_view::npos;
    }
};

/**
 *  @brief Prints the result line of the case @p name and rolls it into @p totals unless the @c --filter leaves it out,
 *      answering its failures so a helper still accumulates.
 *
 *  A failure also prints the command that reruns only this case under the same seed.
 */
inline std::size_t report(suite_totals_t& totals, char const* name, case_result_t result) {
    if (!totals.selects(name)) return 0;
    totals.failures += result.failures;
    if (result.failures != 0) {
        ++totals.failed;
        fmt::println("FAILED  {:<32} : {} failures", name, result.failures);
        fmt::println(stderr, "  rerun: {} --seed {} --filter '^{}$'", totals.program, totals.seed, name);
    }
    else if (result.skip_reason) {
        ++totals.skipped;
        fmt::println("skipped  {:<32} : {}", name, result.skip_reason);
    }
    else {
        ++totals.passed;
        fmt::println("passed  {:<32} : 0 failures", name);
    }
    return result.failures;
}

/**
 *  @brief Runs @p case_function on @p arguments only when the @c --filter selects @p name, then reports it as above.
 *
 *  Takes a function pointer rather than any callable, as SmashTable's harness does, so a case whose signature drifts
 *  fails to compile here instead of hiding behind a lambda at every call site.
 */
template <typename result_type_, typename... parameters_types_>
std::size_t report(suite_totals_t& totals, char const* name, result_type_ (*case_function)(parameters_types_...),
                   std::type_identity_t<parameters_types_>... arguments) {
    if (!totals.selects(name)) return 0;
    return report(totals, name, case_result_t(case_function(arguments...)));
}

/**
 *  @brief Prints the totals and answers the exit code: 1 on any failure or on a @c --filter that matched no case,
 *      3 on a skip the policy refuses, 0 otherwise.
 *
 *  A filter that selected nothing fails, since a mistyped one would otherwise read like a green suite.
 */
[[nodiscard]] inline int suite_exit_code(suite_totals_t const& totals) {
    fmt::println("total {} passed, {} skipped, {} failed, seed {}", totals.passed, totals.skipped, totals.failed,
                 totals.seed);
    if (totals.failures != 0) return 1;
    if (!totals.filter.empty() && totals.passed + totals.skipped + totals.failed == 0) {
        fmt::println(stderr, "--filter '{}' matched no case, so nothing ran", totals.filter);
        return 1;
    }
    if (totals.skipped != 0 && totals.policy == skip_policy_t::refused_k) return 3;
    return 0;
}

/** Counts one failure when @p error is set, printing it, and clears it either way. */
[[nodiscard]] inline std::size_t expect_no_error(ustore_error_t& error) {
    if (!error.status) return 0;
    fmt::println(stderr, "  unexpected error: {}", error.message);
    error = {};
    return 1;
}

/** Counts one failure when @p error is empty, so an invalid call passed, and clears it either way. */
[[nodiscard]] inline std::size_t expect_error(ustore_error_t& error) {
    if (!error.status) {
        fmt::println(stderr, "  an invalid task passed without an error");
        return 1;
    }
    error = {};
    return 0;
}

/**
 *  @brief Bisects a memory budget between zero and @p ceiling, running @p body at every rung.
 *
 *  A rung may refuse only with @c out_of_memory_k, and the caller checks that nothing stays charged afterwards.
 *  Answers the failures counted across the rungs.
 */
template <typename body_type_>
[[nodiscard]] std::size_t under_bisected_budget(std::size_t ceiling, body_type_&& body) {
    std::size_t failures = 0;
    std::size_t refused = 0;
    std::size_t admitted = ceiling;
    while (refused < admitted) {
        std::size_t const budget = refused + (admitted - refused) / 2;
        status_t const status = body(budget);
        if (status.succeeded()) {
            admitted = budget;
            continue;
        }
        if (status.code != status_code_t::out_of_memory_k) {
            fmt::println(stderr, "  a budget of {} bytes failed with {}{}{}", budget, describe(status.code),
                         status.message ? ": " : "", status.message ? status.message : "");
            ++failures;
        }
        refused = budget + 1;
    }
    return failures;
}

/** Spin ceiling for waits on flags a peer sets within bounded work; expiry proves a broken rendezvous. */
constexpr std::size_t spin_limit_k = std::size_t {1} << 28;

/** Spins, yielding, until @p condition holds or @c spin_limit_k expires, and answers whether it held. */
template <typename condition_type_>
[[nodiscard]] inline bool spin_until(condition_type_&& condition) noexcept {
    for (std::size_t spins = 0; spins != spin_limit_k; ++spins) {
        if (condition()) return true;
        fu::preferred_yield_t {}(spins, spins, std::size_t {0});
    }
    return false;
}

#pragma region Flags

/** The reason a case that needs a filesystem reports when the suite ran without @c --directory. */
constexpr char const* needs_directory_k = "needs --directory";

/** A splitmix64 generator, seeded once per suite and forked once per thread, so every draw replays from one seed. */
class test_random_t {
  public:
    explicit constexpr test_random_t(std::uint64_t seed) noexcept : state_(seed) {}

    /** The next 64 random bits. */
    constexpr std::uint64_t next() noexcept {
        std::uint64_t mixed = (state_ += 0x9E3779B97F4A7C15ull);
        mixed = (mixed ^ (mixed >> 30)) * 0xBF58476D1CE4E5B9ull;
        mixed = (mixed ^ (mixed >> 27)) * 0x94D049BB133111EBull;
        return mixed ^ (mixed >> 31);
    }

    /** A random value below @p bound. */
    constexpr std::uint64_t below(std::uint64_t bound) noexcept { return next() % bound; }

    /** The generator of the thread at @p index, which draws a stream distinct from every sibling's. */
    [[nodiscard]] constexpr test_random_t fork(std::size_t index) const noexcept {
        test_random_t branch {state_ + (index + 1) * 0xD1B54A32D192ED03ull};
        return test_random_t {branch.next()};
    }

  private:

    /** The advancing state. */
    std::uint64_t state_;
};

/** The threads the machine offers, at least one. */
[[nodiscard]] inline std::size_t hardware_threads() noexcept {
    unsigned const offered = std::thread::hardware_concurrency();
    return offered != 0 ? offered : 1;
}

/** A non-zero seed from the machine, for a run that asked for none. */
[[nodiscard]] inline std::uint64_t draw_seed() {
    std::random_device device;
    std::uint64_t const drawn = (static_cast<std::uint64_t>(device()) << 32) | device();
    return drawn != 0 ? drawn : 1;
}

/** The flags every suite accepts, parsed from @c argv beside the suite's own. */
struct suite_options_t {

    /** Parent of the unique directory the suite works in, empty to run without persistence. */
    std::string_view directory;

    /** The block device the storage cases open, empty to skip the cases that need one. */
    std::string_view device;

    /** The PCIe address of the NVMe device SPDK binds, empty to skip the cases that need one. */
    std::string_view spdk_address;

    /** The transports a ring suite mounts, every one when empty. */
    texts_t transports;

    /** Fields spliced into the engine's own configuration, like @c "io_transport":"linux-uring", or none. */
    std::string_view engine_config;

    /** The seed every random draw descends from, drawn from the machine when left zero. */
    count_t seed;

    /** Threads the concurrent cases fan out over. */
    count_t threads {hardware_threads(), 1};

    /** Whether a skipped case fails the suite. */
    switch_t no_skips = switch_t::absent_k;

    /** A regex over case labels, like @c ^lsm\.files or @c uring|ram, selecting which cases run. */
    std::string_view filter;

    /** The binary's @c argv[0], which every rerun line starts with. */
    char const* program = "";

    /** A totals seeded, filtered and policed by these options. */
    [[nodiscard]] suite_totals_t totals() const noexcept {
        suite_totals_t counts;
        counts.seed = seed.value;
        counts.policy = no_skips == switch_t::present_k ? skip_policy_t::refused_k : skip_policy_t::tolerated_k;
        counts.filter = filter;
        if (!filter.empty()) {
            try {
                counts.pattern.emplace(filter.begin(), filter.end());
            }
            catch (std::regex_error const&) {
                // Left empty, so `selects` finds the filter as a plain substring instead, as SmashTable does.
            }
        }
        counts.program = program;
        return counts;
    }

    /** A generator seeded by these options. */
    [[nodiscard]] test_random_t random() const noexcept { return test_random_t {seed.value}; }
};

/** Most flags one suite adds to the shared ones. */
constexpr std::size_t suite_flags_capacity_k = 16;

/**
 *  @brief Parses the shared flags and @p suite_flags from @p argv into @p options, drawing a seed when none was given.
 *
 *  Prints the usage on @c --help and the offending argument on failure. A suite flag named like a shared one
 *  shadows it, so a suite may give @c --threads its own meaning.
 */
[[nodiscard]] inline argument_status_t parse_suite_options(int argc, char const* const* argv, suite_options_t& options,
                                                           std::span<flag_t const> suite_flags = {}) {
    if (suite_flags.size() > suite_flags_capacity_k) {
        fmt::println(stderr, "a suite may add at most {} flags", suite_flags_capacity_k);
        return argument_status_t::too_many_items_k;
    }
    std::array<flag_t, suite_flags_capacity_k + 9> flags;
    std::size_t count = 0;
    flags[count++] = {"--directory", "path", "parent of the unique directory the suite works in", &options.directory};
    flags[count++] = {"--device", "path", "block device the storage cases open", &options.device};
    flags[count++] = {"--spdk-address", "address", "PCIe address of the NVMe device SPDK binds", &options.spdk_address};
    flags[count++] = {"--transports", "names", "transports a ring suite mounts, like memory,blocking,linux-uring",
                      &options.transports};
    flags[count++] = {"--engine-config", "json",
                      "fields of the engine's own configuration, like \"io_transport\":\"uring\"",
                      &options.engine_config};
    flags[count++] = {"--seed", "count", "seed of every random draw, drawn from the machine when absent",
                      &options.seed};
    flags[count++] = {"--threads", "count", "threads the concurrent cases fan out over", &options.threads};
    flags[count++] = {"--no-skips", "", "fail the suite when a case skips", &options.no_skips};
    flags[count++] = {"--filter", "regex", "runs only the cases whose label it matches, like '^lsm\\.files'",
                      &options.filter};
    for (flag_t const& flag : suite_flags) flags[count++] = flag;

    std::span<flag_t const> const every_flag {flags.data(), count};
    arguments_t const parsed = parse_arguments(argc, argv, every_flag, {});
    if (parsed.status == argument_status_t::help_k) print_usage(argv[0], every_flag);
    else if (parsed.status != argument_status_t::parsed_k)
        fmt::println(stderr, "{}: {}", parsed.argument, describe(parsed.status));
    if (options.seed.value == 0) options.seed.value = draw_seed();
    options.program = argc != 0 ? argv[0] : "";
    return parsed.status;
}

#pragma endregion Flags

#pragma region Directories

/** Longest path the tests create. */
constexpr std::size_t path_capacity_k = 4096;

/** A path of one name under one directory, held inline. */
struct test_path_t {

    /** The null-terminated path. */
    char text[path_capacity_k] {};

    /** Joins @p directory and @p name with a slash, truncated to fit. */
    test_path_t(char const* directory, char const* name) noexcept {
        auto const written = fmt::format_to_n(text, sizeof(text) - 1, "{}/{}", directory, name);
        *written.out = '\0';
    }

    /** The path as a C string. */
    char const* c_str() const noexcept { return text; }
};

/** An engine configuration held inline, empty when the engine should use its defaults. */
class test_config_t {
  public:

    /** Configures the engine to work in @p directory, with @p engine_config spliced into the engine's own object. */
    explicit test_config_t(char const* directory, std::string_view engine_config = {}) noexcept
        : directory_(directory), engine_config_(engine_config) {
        if (!directory && engine_config.empty()) return;
        auto const written =
            engine_config.empty()
                ? fmt::format_to_n(json_, sizeof(json_) - 1, R"({{"version": "1.0", "directory": "{}"}})", directory)
            : !directory
                ? fmt::format_to_n(json_, sizeof(json_) - 1,
                                   R"({{"version": "1.0", "engine": {{"config": {{"version": "1.0", {}}}}}}})",
                                   engine_config)
                : fmt::format_to_n(
                      json_, sizeof(json_) - 1,
                      R"({{"version": "1.0", "directory": "{}", "engine": {{"config": {{"version": "1.0", {}}}}}}})",
                      directory, engine_config);
        *written.out = '\0';
    }

    /** The configuration as a C string, empty for the defaults. */
    [[nodiscard]] char const* c_str() const noexcept { return json_; }

    /** The directory the engine works in, or null without @c --directory. */
    [[nodiscard]] char const* directory() const noexcept { return directory_; }

    /** The fields spliced into the engine's own configuration, which a second database of a case repeats. */
    [[nodiscard]] std::string_view engine_config() const noexcept { return engine_config_; }

  private:

    /** The null-terminated JSON configuration, empty for the defaults. */
    char json_[path_capacity_k + 256] {};

    /** The directory the engine works in, or null. */
    char const* directory_ = nullptr;

    /** The fields spliced into the engine's own configuration. */
    std::string_view engine_config_;
};

/** Removes one entry of a depth-first walk. */
inline int remove_entry(char const* entry, struct stat const*, int, struct FTW*) noexcept { return std::remove(entry); }

/** Creates @p directory with its missing parents, answering whether it exists. */
[[nodiscard]] inline bool create_directories(char const* directory) noexcept {
    std::size_t const length = std::strlen(directory);
    if (length >= path_capacity_k) return false;
    char prefix[path_capacity_k];
    std::memcpy(prefix, directory, length + 1);
    for (std::size_t index = 1; index <= length; ++index) {
        char const separator = prefix[index];
        if (separator != '/' && separator != '\0') continue;
        prefix[index] = '\0';
        if (mkdir(prefix, 0755) != 0 && errno != EEXIST) return false;
        prefix[index] = separator;
    }
    return true;
}

/** Removes @p directory with its contents and creates it again with its parents, answering whether it exists. */
[[nodiscard]] inline bool recreate_directory(char const* directory) noexcept {
    [[maybe_unused]] int const removed = nftw(directory, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
    return create_directories(directory);
}

/**
 *  @brief A unique directory under the parent @c --directory names, removed with its contents on destruction.
 *
 *  Without a parent it names no path, and the cases that need persistence report skipped.
 */
class test_directory_t {
  public:
    test_directory_t(test_directory_t&& other) noexcept {
        std::memcpy(path_, other.path_, sizeof(path_));
        other.path_[0] = '\0';
    }

    test_directory_t& operator=(test_directory_t&&) = delete;
    test_directory_t(test_directory_t const&) = delete;
    test_directory_t& operator=(test_directory_t const&) = delete;

    ~test_directory_t() noexcept {
        if (path_[0] != '\0') {
            [[maybe_unused]] int const removed = nftw(path_, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
        }
    }

    /** Creates @p parent with its parents and a unique directory inside it, or names none for an empty @p parent. */
    [[nodiscard]] static st::expected<test_directory_t> make(std::string_view parent) noexcept {
        using st::status_t;
        test_directory_t directory;
        if (parent.empty()) return directory;

        constexpr std::string_view unique_suffix_k = "/ustore-XXXXXX";
        if (parent.size() + unique_suffix_k.size() >= path_capacity_k) return status_t::invalid_argument_k;
        std::memcpy(directory.path_, parent.data(), parent.size());
        std::memcpy(directory.path_ + parent.size(), unique_suffix_k.data(), unique_suffix_k.size() + 1);
        directory.path_[parent.size()] = '\0';
        bool const created = create_directories(directory.path_);
        directory.path_[parent.size()] = '/';
        if (!created || !mkdtemp(directory.path_)) {
            status_t const status = static_cast<status_t>(errno);
            directory.path_[0] = '\0';
            return status;
        }
        return directory;
    }

    /** The unique directory, or null without @c --directory. */
    [[nodiscard]] char const* path() const noexcept { return path_[0] != '\0' ? path_ : nullptr; }

  private:
    test_directory_t() noexcept = default;

    /** The null-terminated path, empty when the suite runs without a directory. */
    char path_[path_capacity_k] {};
};

#pragma endregion Directories

} // namespace unum::ustore::test
