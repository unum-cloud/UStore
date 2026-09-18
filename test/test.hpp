/**
 *  @file test/test.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief Shared checks for the C++ test suites: failure counting, verdicts, bounded waits, flags and directories.
 *
 *  Every check returns its own failure count, and counts roll up through each test function to
 *  @c main without global mutable state. Four verbs cover every case: @c expect for a condition,
 *  @c expect_eq and @c expect_near when diagnosis needs the values, and @c report for the verdict.
 */
#pragma once

#include <cerrno>     // `errno`, `EEXIST`
#include <cmath>      // `std::fabs`, `std::fmax`
#include <cstddef>    // `std::size_t`
#include <cstdio>     // `stderr`, `std::remove`
#include <cstdlib>    // `mkdtemp`
#include <cstring>    // `std::memcpy`, `std::strlen`
#include <ftw.h>      // `nftw`
#include <sys/stat.h> // `mkdir`

#include <span>        // `std::span`
#include <string_view> // `std::string_view`
#include <thread>      // `std::this_thread::yield`

#include <fmt/format.h>

#include <smashtable/shared.hpp>

#include <ustore/db.h>
#include <ustore/types.hpp>

#include "arguments.hpp"

namespace unum::ustore {

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

/** Prints one verdict line and answers the count it was given, so a caller accumulates through it. */
inline std::size_t report(char const* name, std::size_t failures) {
    fmt::println("{}  {:<32} : {} failures", failures ? "FAILED" : "passed", name, failures);
    return failures;
}

/** Prints a skipped case with its reason; a skip counts no failures. */
inline std::size_t report(char const* name, char const* skip_reason) {
    fmt::println("skipped  {:<32} : {}", name, skip_reason);
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

/** Spin ceiling for waits on flags a peer sets within bounded work; expiry proves a broken rendezvous. */
constexpr std::size_t spin_limit_k = std::size_t {1} << 28;

/** Spins, yielding, until @p condition holds or @c spin_limit_k expires, and answers whether it held. */
template <typename condition_type_>
[[nodiscard]] inline bool spin_until(condition_type_&& condition) noexcept {
    for (std::size_t spins = 0; spins != spin_limit_k; ++spins) {
        if (condition()) return true;
        std::this_thread::yield();
    }
    return false;
}

#pragma region Flags

/** The reason a case that needs a filesystem reports when the suite ran without @c --directory. */
constexpr char const* needs_directory_k = "needs --directory";

/** The @c --directory flag every suite accepts, writing the parent it names into @p parent. */
[[nodiscard]] inline flag_t directory_flag(std::string_view& parent) noexcept {
    return {"--directory", "path", "parent of the unique directory the suite works in", &parent};
}

/** Parses @p flags from @p argv, printing the usage on @c --help and the offending argument on failure. */
[[nodiscard]] inline argument_status_t parse_test_arguments(int argc, char const* const* argv,
                                                            std::span<flag_t const> flags) {
    arguments_t const parsed = parse_arguments(argc, argv, flags, {});
    if (parsed.status == argument_status_t::help_k) print_usage(argv[0], flags);
    else if (parsed.status != argument_status_t::parsed_k)
        fmt::println(stderr, "{}: {}", parsed.argument, describe(parsed.status));
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
struct test_config_t {

    /** The null-terminated JSON configuration, empty for the defaults. */
    char json[path_capacity_k + 64] {};

    /** Configures the engine to work in @p directory, or leaves the defaults for a null one. */
    explicit test_config_t(char const* directory) noexcept {
        if (!directory) return;
        auto const written = fmt::format_to_n(json, sizeof(json) - 1, R"({{"version": "1.0", "directory": "{}"}})",
                                              directory);
        *written.out = '\0';
    }

    /** The configuration as a C string. */
    char const* c_str() const noexcept { return json; }
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

} // namespace unum::ustore
