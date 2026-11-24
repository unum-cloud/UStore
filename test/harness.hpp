/**
 *  @file test/harness.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief Shared checks for the C++ test suites: the run and its settings, the checks, bounded
 *      waits and directories.
 *
 *  Every check counts its failure into @c test_run, the harness's one sanctioned mutable global,
 *  and @c run_test_case runs each case the filter selects and prints its verdict, so
 *  @c finish_test_run answers the exit code without a tally threaded through the cases.
 */
#pragma once

#include <cmath>   // `std::fabs`, `std::fmax`, `std::llround`
#include <csignal> // `std::signal`, `std::raise`, `SIGTRAP`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint8_t`, `std::uint64_t`
#include <cstdio>  // `std::FILE`, `std::fflush`, `stderr`
#include <cstdlib> // `std::getenv`, `std::exit`
#include <cstring> // `std::memcpy`, `std::strcmp`

#include <algorithm>        // `std::max`
#include <atomic>           // `std::atomic`
#include <charconv>         // `std::from_chars`
#include <chrono>           // `std::chrono::steady_clock`
#include <concepts>         // `std::convertible_to`
#include <filesystem>       // `std::filesystem::create_directories`, `std::filesystem::remove_all`
#include <functional>       // `std::invoke`
#include <initializer_list> // `std::initializer_list`
#include <random>           // `std::random_device`
#include <regex>            // `std::regex`, `std::regex_search`
#include <source_location>  // `std::source_location`
#include <string_view>      // `std::string_view`
#include <system_error>     // `std::error_code`, `std::errc`
#include <thread>           // `std::thread::hardware_concurrency`, `std::this_thread::yield`
#include <type_traits>      // `std::invoke_result_t`, `std::is_void_v`, `std::type_identity_t`
#include <utility>          // `std::move`, `std::forward`

#include <fmt/format.h>

#include <smashtable/shared.hpp>
#include <stringzilla/stringzilla.hpp> // `sz::status_t`, `sz::strings_view_t`

#include "ustore/db.h"
#include "ustore/utilities/types.hpp" // `status_t`, `expected`, `uuid_to_integer`, `uuid_to_text`

#include "arguments.hpp"

#if USTORE_OS_POSIX_
#include <unistd.h> // `write`, `STDERR_FILENO`
#endif
#if (defined(__GLIBC__) || USTORE_OS_APPLE_) && __has_include(<execinfo.h>)
#include <execinfo.h> // `backtrace`, `backtrace_symbols_fd`
#endif

/** Prints a UUID as the integer it spells when its upper half is zero, else as canonical text. */
template <>
struct fmt::formatter<ustore_uuid_t> : fmt::formatter<std::string_view> {
    auto format(ustore_uuid_t const& uuid, fmt::format_context& context) const {
        using namespace unum::ustore;
        if (auto [integer, integer_status] = util::uuid_to_integer(uuid); !integer_status.failed())
            return fmt::format_to(context.out(), "{}", integer);
        char text[util::uuid_text_length_k];
        util::uuid_to_text(uuid, text);
        return fmt::formatter<std::string_view>::format(std::string_view(text, sizeof(text)), context);
    }
};

template <>
struct fmt::formatter<ustore_key_t> : fmt::formatter<ustore_uuid_t> {
    auto format(ustore_key_t const& key, fmt::format_context& context) const {
        return fmt::formatter<ustore_uuid_t>::format(key.uuid, context);
    }
};

template <>
struct fmt::formatter<ustore_collection_id_t> : fmt::formatter<ustore_uuid_t> {
    auto format(ustore_collection_id_t const& collection, fmt::format_context& context) const {
        return fmt::formatter<ustore_uuid_t>::format(collection.uuid, context);
    }
};

namespace unum::ustore::test {

using namespace tools;

/** Stops in an attached debugger, or dumps core without one; SIGTRAP stays outside the
 *  crash handlers. */
inline void trap_into_debugger() noexcept {
#if USTORE_OS_WINDOWS_
    __debugbreak();
#else
    std::raise(SIGTRAP);
#endif
}

/** Prints the case in progress and a backtrace when the process dies of a fatal signal. */
inline void install_test_signal_handlers() noexcept;

/** Writes one line to @p stream, formatted from @p format and @p args, then its newline. */
template <typename... args_types_>
void print_line(std::FILE* stream, fmt::format_string<args_types_...> format, args_types_&&... args) {
    fmt::println(stream, format, std::forward<args_types_>(args)...);
}

#pragma region Shared Settings

/** Exits with one line naming a setting that does not parse, so a bad value never runs anything. */
[[noreturn]] inline void exit_unparsed(char const* variable, char const* text, char const* expected) noexcept {
    print_line(stderr, "{}=\"{}\" does not parse, expected {}", variable, text, expected);
    std::exit(1);
}

/** The text of a setting: its @p flag when given, else its environment @p variable , else null. */
inline char const* setting_text(int argc, char** argv, char const* variable, std::string_view flag) noexcept {
    for (int index = 1; index < argc; ++index) {
        std::string_view const argument {argv[index]};
        if (argument == flag)
            return index + 1 < argc && std::strncmp(argv[index + 1], "--", 2) != 0 ? argv[index + 1] : "1";
        if (argument.size() > flag.size() && argument.starts_with(flag) && argument[flag.size()] == '=')
            return argv[index] + flag.size() + 1;
    }
    char const* text = std::getenv(variable);
    return text && *text ? text : nullptr;
}

/** A switch, @p fallback when unset: 0, false, off and no turn it off, anything else on. */
inline bool read_flag_setting(int argc, char** argv, char const* variable, std::string_view flag,
                              bool fallback = false) noexcept {
    char const* text = setting_text(argc, argv, variable, flag);
    if (!text) return fallback;
    for (char const* off : {"0", "false", "off", "no"})
        if (std::strcmp(text, off) == 0) return false;
    return true;
}

/** A whole number, @p fallback when unset. */
inline std::size_t read_count_setting(int argc, char** argv, char const* variable, std::string_view flag,
                                      std::size_t fallback) noexcept {
    char const* text = setting_text(argc, argv, variable, flag);
    if (!text) return fallback;
    std::string_view const spelled {text};
    std::size_t count = 0;
    auto const [end, parse_status] = std::from_chars(spelled.data(), spelled.data() + spelled.size(), count);
    if (parse_status != std::errc {} || end != spelled.data() + spelled.size())
        exit_unparsed(variable, text, "a whole number");
    return count;
}

/** A positive real, @p fallback when unset. */
inline double read_real_setting(int argc, char** argv, char const* variable, std::string_view flag,
                                double fallback) noexcept {
    char const* text = setting_text(argc, argv, variable, flag);
    if (!text) return fallback;
    char* end = nullptr;
    double const value = std::strtod(text, &end);
    if (*end != '\0' || !(value > 0) || !std::isfinite(value))
        exit_unparsed(variable, text, "a positive number like 0.1 or 10");
    return value;
}

/** A positive duration such as 200ms, 10s or 2m, @p fallback when unset. */
inline std::chrono::milliseconds read_duration_setting(int argc, char** argv, char const* variable,
                                                       std::string_view flag,
                                                       std::chrono::milliseconds fallback) noexcept {
    char const* text = setting_text(argc, argv, variable, flag);
    if (!text) return fallback;
    std::string_view const spelled {text};
    std::uint32_t count = 0;
    auto const [end, parse_status] = std::from_chars(spelled.data(), spelled.data() + spelled.size(), count);
    std::string_view const unit = spelled.substr(static_cast<std::size_t>(end - spelled.data()));
    if (parse_status == std::errc {} && count != 0) {
        if (unit == "ms") return std::chrono::milliseconds(count);
        if (unit == "s") return std::chrono::seconds(count);
        if (unit == "m") return std::chrono::minutes(count);
    }
    exit_unparsed(variable, text, "a duration like 200ms, 10s or 2m");
}

/** The run's seed: 42 when unset, drawn afresh for @c random , else any unsigned 64-bit value. */
inline std::uint64_t read_seed_setting(int argc, char** argv, char const* variable, std::string_view flag) noexcept {
    char const* text = setting_text(argc, argv, variable, flag);
    if (!text) return 42;
    std::string_view const spelled {text};
    if (spelled == "random") {
        std::random_device device;
        return (static_cast<std::uint64_t>(device()) << 32) ^ device();
    }
    std::uint64_t seed = 0;
    auto const [end, parse_status] = std::from_chars(spelled.data(), spelled.data() + spelled.size(), seed);
    if (parse_status != std::errc {} || end != spelled.data() + spelled.size())
        exit_unparsed(variable, text, "an unsigned integer or random");
    return seed;
}

/** Which labels a run selects: those its regex matches somewhere, every label while unset. */
struct regex_filter_t {
    std::string_view text;
    std::regex regex {""};

    bool selects(std::string_view label) const noexcept { return std::regex_search(label.begin(), label.end(), regex); }
};

/** The filter a setting spells, compiled once; one that does not compile exits as a bad setting. */
inline regex_filter_t read_regex_filter_setting(int argc, char** argv, char const* variable,
                                                std::string_view flag) noexcept {
    regex_filter_t filter;
    char const* text = setting_text(argc, argv, variable, flag);
    if (!text) return filter;
    filter.text = text;
#if defined(__cpp_exceptions) && __cpp_exceptions
    try {
        filter.regex.assign(filter.text.begin(), filter.text.end());
    }
    catch (std::regex_error const&) {
        exit_unparsed(variable, text, "a regular expression");
    }
#else
    filter.regex.assign(filter.text.begin(), filter.text.end());
#endif
    return filter;
}

/** What every suite and bench reads before its own settings; @c program starts the rerun lines. */
struct common_settings_t {
    std::uint64_t seed = 42;
    regex_filter_t regex_filter;
    char const* program = "";
};

/** SplitMix64's finalizer, a bijection that spreads every input bit over all 64 output bits. */
constexpr std::uint64_t split_mix_bits(std::uint64_t value) noexcept {
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

/** The @p index -th stream of the family @p name under @p seed , identical in every language. */
constexpr std::uint64_t stream_key(std::uint64_t seed, std::string_view name, std::uint64_t index = 0) noexcept {
    std::uint64_t hashed = 0xCBF29CE484222325ull;
    for (char const character : name) hashed = (hashed ^ static_cast<std::uint8_t>(character)) * 0x100000001B3ull;
    return split_mix_bits(split_mix_bits(seed ^ hashed) + index);
}

/**
 *  @brief SplitMix64 over one 64-bit counter, seeded by a @ref stream_key , bit-identical to the
 *      JavaScript generator NumKong ships.
 *
 *  Distributions are not specified to draw the same sequence on every standard library, so a
 *  failure would stop reproducing from its seed when the toolchain changed; @ref below is the
 *  portable bounded draw.
 */
struct split_mix_generator_t {
    using result_type = std::uint64_t;
    std::uint64_t counter = 0;

    explicit split_mix_generator_t(std::uint64_t key) noexcept : counter(key) {}
    static constexpr result_type(min)() noexcept { return 0; }
    static constexpr result_type(max)() noexcept { return ~result_type {0}; }
    result_type operator()() noexcept { return split_mix_bits(counter += 0x9E3779B97F4A7C15ull); }

    /** A uniform draw below @p bound , zero for an empty bound, without the bias of a modulo. */
    std::uint64_t below(std::uint64_t bound) noexcept {
        if (bound == 0) return 0;
        std::uint64_t const rejection_floor = (0u - bound) % bound;
        std::uint64_t draw = (*this)();
        while (draw < rejection_floor) draw = (*this)();
        return draw % bound;
    }
};

#pragma endregion Shared Settings

/** The threads the machine offers, at least one. */
[[nodiscard]] inline std::size_t hardware_threads() noexcept {
    unsigned const offered = std::thread::hardware_concurrency();
    return offered != 0 ? offered : 1;
}

/** What a suite prints above its settings: the library it tests and the machine it runs on. */
struct machine_t {
    std::size_t threads = 1;
};

/** Probes the facts @c machine_t reports. */
inline machine_t probe_machine() noexcept { return {hardware_threads()}; }

/** Prints the version line, the platform the suite was compiled for, and the threads this
 *  machine offers. */
inline void print(machine_t const& machine) noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    char const* const architecture = "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    char const* const architecture = "arm64";
#elif defined(__riscv) && __riscv_xlen == 64
    char const* const architecture = "riscv64";
#elif defined(__powerpc64__)
    char const* const architecture = "ppc64";
#elif defined(__wasm__)
    char const* const architecture = "wasm";
#else
    char const* const architecture = "an unlisted architecture";
#endif
#if USTORE_OS_LINUX_
    char const* const system = "Linux";
#elif USTORE_OS_APPLE_
    char const* const system = "macOS";
#elif USTORE_OS_WINDOWS_
    char const* const system = "Windows";
#elif USTORE_OS_FREEBSD_
    char const* const system = "FreeBSD";
#else
    char const* const system = "an unlisted system";
#endif
    fmt::println("UStore {}.{}.{}", USTORE_VERSION_MAJOR, USTORE_VERSION_MINOR, USTORE_VERSION_PATCH);
    fmt::println("- Compiled for: {} {}", architecture, system);
    fmt::println("- This machine: {} threads", machine.threads);
}

/** What every suite reads: the run's own settings, then the paths, transports and engine the
 *  storage cases open. */
struct test_settings_t : common_settings_t {
    double scale = 1.0;
    bool trap_on_failure = false;
    bool refuse_skips = false;

    /** Parent of the unique directory the suite works in, empty to run without persistence. */
    std::string_view directory;

    /** The block device the storage cases open, empty to skip the cases that need one. */
    std::string_view device;

    /** The PCIe address of the NVMe device SPDK binds, empty to skip the cases that need one. */
    std::string_view spdk_address;

    /** The transports a ring suite mounts, every one when empty. */
    texts_t transports;

    /** The engine an engine suite opens, with the isolation and the transport of its init call. */
    engine_options_t engine;

    /** Threads the concurrent cases fan out over. */
    std::size_t threads = hardware_threads();
};

/** Reads every @c test_settings_t field, each from its <tt>--flag</tt> or its @c
 *  USTORE_ variable. */
inline test_settings_t read_test_settings(int argc, char** argv) noexcept;

#pragma region Shared Run

/** What a case reports beyond its checks: that it ran, or why it skipped. */
struct case_result_t {
    char const* skip_reason = nullptr;

    [[nodiscard]] static constexpr case_result_t skipped(char const* reason) noexcept { return {reason}; }
};

/**
 *  @brief The run in progress: the harness's one sanctioned mutable global.
 *
 *  Checks fire on worker threads and inside helpers as well as in a case's own body, so the run
 *  counts failed checks in one place and @ref run_test_case diffs that count around each case,
 *  rather than threading a tally through every call. After a crash, printing @c test_run from a
 *  core dump names the case in progress and the last failed check, and the signal handlers print
 *  the case too.
 */
struct test_run_t {
    test_settings_t settings {};
    char const* current_case = nullptr;
    std::atomic<std::size_t> failed_checks {0};
    std::atomic<char const*> last_failed_file {nullptr};
    std::atomic<std::uint_least32_t> last_failed_line {0};
    std::size_t passed_cases = 0;
    std::size_t failed_cases = 0;
    std::size_t skipped_cases = 0;
};

inline test_run_t test_run;

/** How long any wait in a test may take; a passing case never waits it out, so it bounds hangs. */
inline constexpr std::chrono::seconds wait_limit_k {120};

/** Yields until @p predicate holds; false when @ref wait_limit_k passed first. */
template <typename predicate_type_>
bool spin_until(predicate_type_&& predicate) noexcept {
    auto const deadline = std::chrono::steady_clock::now() + wait_limit_k;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

/** @p baseline iterations under the @c SCALE setting, never fewer than one. */
inline std::size_t scaled_iterations(std::size_t baseline) noexcept {
    double const scaled = static_cast<double>(baseline) * test_run.settings.scale;
    return scaled < 1 ? 1 : static_cast<std::size_t>(std::llround(scaled));
}

/** The step that visits a @p coverage fraction of a sweep, at least one. */
inline std::size_t sweep_stride(double coverage) noexcept {
    double const stride = 1.0 / coverage;
    return stride < 1 ? 1 : static_cast<std::size_t>(std::llround(stride));
}

/** The @p index -th stream of the case in progress, so a filtered run draws what a full one did. */
inline std::uint64_t case_stream_key(std::uint64_t index = 0) noexcept {
    return stream_key(test_run.settings.seed, test_run.current_case ? test_run.current_case : "", index);
}

/** Line-buffers stdout, prints the machine, reads the settings, arms the crash handlers and
 *  prints the settings line, before the first case. */
inline void start_test_run(int argc, char** argv) noexcept {
    std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ); // before any output, or the C library keeps its buffering
    print(probe_machine());
    test_run.settings = read_test_settings(argc, argv);
    install_test_signal_handlers();
    print_line(stdout, "- Seed: {}, scale: {}", test_run.settings.seed, test_run.settings.scale);
    print_line(
        stdout, "- Filter: {}",
        test_run.settings.regex_filter.text.empty() ? std::string_view("none") : test_run.settings.regex_filter.text);
}

/** Counts one failed check at @p where , and traps when the run asked to stop at the first. */
inline void record_failed_check(std::source_location where) noexcept {
    std::fflush(stdout); // so a log that merges both streams keeps the failure under its case
    print_line(stderr, "  check failed at {}:{}", where.file_name(), where.line());
    test_run.last_failed_file.store(where.file_name(), std::memory_order_relaxed);
    test_run.last_failed_line.store(where.line(), std::memory_order_relaxed);
    test_run.failed_checks.fetch_add(1, std::memory_order_relaxed);
    if (test_run.settings.trap_on_failure) trap_into_debugger();
}

/** Holds when @p is_satisfied does; otherwise counts a failed check at the caller's line. */
inline bool expect_true(bool is_satisfied, std::source_location where = std::source_location::current()) noexcept {
    if (!is_satisfied) record_failed_check(where);
    return is_satisfied;
}

/** A case label spelled as a regex that matches only itself, every metacharacter escaped. */
struct regex_escaped_t {
    std::string_view label;
};

} // namespace unum::ustore::test

/** The short spelling the foreign specializations below reach the project's types through. */
namespace us = unum::ustore;

/** Writes a @ref regex_escaped_t straight to the output, a backslash before each metacharacter. */
template <>
struct fmt::formatter<us::test::regex_escaped_t> {
    constexpr auto parse(fmt::format_parse_context& context) { return context.begin(); }
    template <typename context_type_>
    auto format(us::test::regex_escaped_t const& escaped, context_type_& context) const {
        auto output = context.out();
        for (char const character : escaped.label) {
            if (std::string_view {"\\^$.|?*+()[]{}"}.find(character) != std::string_view::npos) *output++ = '\\';
            *output++ = character;
        }
        return output;
    }
};

namespace unum::ustore::test {

/** Prints one case's verdict, and the line that reruns it alone when it failed. */
inline void report_test_case(char const* label, std::size_t failed_checks, char const* skip_reason,
                             double seconds) noexcept {
    if (skip_reason) {
        print_line(stdout, "- {} ... skipped: {}", label, skip_reason);
        return;
    }
    if (failed_checks == 0) {
        print_line(stdout, "- {} ... ok ({:.2f} s)", label, seconds);
        return;
    }
    std::fflush(stdout);
    print_line(stderr, "- {} ... FAILED: {} failed check(s)", label, failed_checks);
    print_line(stderr, "  rerun: {} --seed {} --filter '^{}$'", test_run.settings.program, test_run.settings.seed,
               regex_escaped_t {label});
}

/** Runs one case unless the filter leaves it out, and records whether it passed or skipped. */
template <typename case_function_type_, typename... argument_types_>
void run_test_case(char const* label, case_function_type_&& case_function, argument_types_&&... arguments) noexcept {
    if (!test_run.settings.regex_filter.selects(label)) return;
    test_run.current_case = label;
    std::size_t const failed_before = test_run.failed_checks.load();
    auto const started = std::chrono::steady_clock::now();
    case_result_t result {};
    using returned_type = std::invoke_result_t<case_function_type_, argument_types_...>;
    if constexpr (std::is_void_v<returned_type>)
        std::invoke(case_function, std::forward<argument_types_>(arguments)...);
    else result = std::invoke(case_function, std::forward<argument_types_>(arguments)...);
    std::chrono::duration<double> const elapsed = std::chrono::steady_clock::now() - started;
    std::size_t const failed_checks = test_run.failed_checks.load() - failed_before;
    if (result.skip_reason) ++test_run.skipped_cases;
    else if (failed_checks != 0) ++test_run.failed_cases;
    else ++test_run.passed_cases;
    report_test_case(label, failed_checks, result.skip_reason, elapsed.count());
    test_run.current_case = nullptr;
}

/** The exit code: 1 when a case or a check outside any case failed, or nothing ran; 3 when
 *  skips are refused and one happened; else 0. */
inline int finish_test_run() noexcept {
    print_line(stdout, "{} passed, {} failed, {} skipped, seed {}", test_run.passed_cases, test_run.failed_cases,
               test_run.skipped_cases, test_run.settings.seed);
    bool const failed = test_run.failed_cases != 0 || test_run.failed_checks.load() != 0;
    if (failed || test_run.passed_cases + test_run.skipped_cases == 0) return 1;
    if (test_run.skipped_cases != 0 && test_run.settings.refuse_skips) return 3;
    return 0;
}

#pragma endregion Shared Run

inline test_settings_t read_test_settings(int argc, char** argv) noexcept {
    test_settings_t settings;
    settings.seed = read_seed_setting(argc, argv, "USTORE_SEED", "--seed");
    settings.regex_filter = read_regex_filter_setting(argc, argv, "USTORE_FILTER", "--filter");
    settings.program = argc != 0 ? argv[0] : "";
    settings.scale = read_real_setting(argc, argv, "USTORE_SCALE", "--scale", 1.0);
    settings.trap_on_failure = read_flag_setting(argc, argv, "USTORE_TRAP_ON_FAILURE", "--trap-on-failure");
    settings.refuse_skips = read_flag_setting(argc, argv, "USTORE_NO_SKIPS", "--no-skips");
    if (char const* text = setting_text(argc, argv, "USTORE_DIRECTORY", "--directory")) settings.directory = text;
    if (char const* text = setting_text(argc, argv, "USTORE_DEVICE", "--device")) settings.device = text;
    if (char const* text = setting_text(argc, argv, "USTORE_SPDK_ADDRESS", "--spdk-address"))
        settings.spdk_address = text;
    if (char const* text = setting_text(argc, argv, "USTORE_TRANSPORTS", "--transports");
        text && parse_texts(text, settings.transports) != argument_status_t::parsed_k)
        exit_unparsed("USTORE_TRANSPORTS", text, "names like memory,blocking,linux-uring");
    if (char const* text = setting_text(argc, argv, "USTORE_ENGINE", "--engine")) settings.engine.name = text;
    if (char const* text = setting_text(argc, argv, "USTORE_ISOLATION", "--isolation"))
        settings.engine.isolation = text;
    if (char const* text = setting_text(argc, argv, "USTORE_ENGINE_TRANSPORT", "--engine-transport"))
        settings.engine.transport = text;
    settings.threads = read_count_setting(argc, argv, "USTORE_THREADS", "--threads", settings.threads);
    return settings;
}

/** The name of the fatal @p signal_number a crash line reports. */
inline char const* signal_name_(int signal_number) noexcept {
    switch (signal_number) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
#if defined(SIGBUS)
    case SIGBUS: return "SIGBUS";
#endif
    }
    return "a fatal signal";
}

/** Writes @p text to stderr without locking stdio, which the crashing thread may already hold. */
inline void write_unbuffered_(char const* text) noexcept {
#if USTORE_OS_WINDOWS_
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
#else
    [[maybe_unused]] auto const written = ::write(STDERR_FILENO, text, std::strlen(text));
#endif
}

/** Names the case in progress, prints a backtrace where @c execinfo exists, and re-raises, so a
 *  crash names its case and still dumps core. */
inline void test_fatal_signal_handler_(int signal_number) noexcept {
    write_unbuffered_("\n*** ");
    write_unbuffered_(signal_name_(signal_number));
    write_unbuffered_(" in case '");
    write_unbuffered_(test_run.current_case ? test_run.current_case : "none");
    write_unbuffered_("' - backtrace follows ***\n");
#if (defined(__GLIBC__) || USTORE_OS_APPLE_) && __has_include(<execinfo.h>)
    void* frames[64];
    int const frames_count = backtrace(frames, static_cast<int>(sizeof(frames) / sizeof(frames[0])));
    backtrace_symbols_fd(frames, frames_count, STDERR_FILENO);
#endif
    std::signal(signal_number, SIG_DFL);
    std::raise(signal_number);
}

#if USTORE_OS_WINDOWS_

/** The Windows twin of @c test_fatal_signal_handler_ for structured exceptions, which no signal
 *  reports. */
inline LONG WINAPI test_fatal_exception_filter_(EXCEPTION_POINTERS*) noexcept {
    write_unbuffered_("\n*** a structured exception in case '");
    write_unbuffered_(test_run.current_case ? test_run.current_case : "none");
    write_unbuffered_("' ***\n");
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

inline void install_test_signal_handlers() noexcept {
#if USTORE_OS_WINDOWS_
    SetUnhandledExceptionFilter(test_fatal_exception_filter_);
#endif
    for (int const signal_number : {SIGSEGV, SIGABRT, SIGILL, SIGFPE})
        std::signal(signal_number, test_fatal_signal_handler_);
#if defined(SIGBUS)
    std::signal(SIGBUS, test_fatal_signal_handler_);
#endif
}

/**
 *  @brief A format string bundled with the @c std::source_location of the call that spelled it.
 *
 *  The implicit conversion from a string literal captures the caller's line behind the variadic
 *  arguments of a check's context, where a defaulted trailing parameter cannot go, and checks the
 *  pattern against those arguments at compile time, as @c fmt::format_string does.
 */
template <typename... args_types_>
struct located_format {
    fmt::format_string<args_types_...> format;
    std::source_location location;

    template <typename text_type_>
    consteval located_format(text_type_ const& text,
                             std::source_location location = std::source_location::current()) noexcept
        : format(text), location(location) {}
};

/** Prints @p context formatted with @p args , indented under a failed check's location line. */
template <typename... args_types_>
void print_context_(located_format<std::type_identity_t<args_types_>...> context, args_types_ const&... args) noexcept {
    fmt::print(stderr, "    ");
    fmt::vprint(stderr, context.format.get(), fmt::make_format_args(args...));
    fmt::print(stderr, "\n");
}

/** Holds when @p is_satisfied does; otherwise counts a failed check at the caller's line and prints
 *  @p context formatted with @p args beneath it. */
template <typename... args_types_>
bool expect_true(bool is_satisfied, located_format<std::type_identity_t<args_types_>...> context,
                 args_types_ const&... args) noexcept {
    if (is_satisfied) return true;
    record_failed_check(context.location);
    print_context_<args_types_...>(context, args...);
    return false;
}

/** Holds when @p actual equals @p wanted ; otherwise counts a failed check and prints both. */
template <typename actual_type_, typename wanted_type_>
bool expect_equal(actual_type_ const& actual, wanted_type_ const& wanted,
                  std::source_location where = std::source_location::current()) noexcept {
    if (actual == wanted) return true;
    record_failed_check(where);
    fmt::println(stderr, "    expected {}, got {}", wanted, actual);
    return false;
}

/** The same, printing @p context with @p args beneath both values. */
template <typename actual_type_, typename wanted_type_, typename... args_types_>
bool expect_equal(actual_type_ const& actual, wanted_type_ const& wanted,
                  located_format<std::type_identity_t<args_types_>...> context, args_types_ const&... args) noexcept {
    if (actual == wanted) return true;
    record_failed_check(context.location);
    fmt::println(stderr, "    expected {}, got {}", wanted, actual);
    print_context_<args_types_...>(context, args...);
    return false;
}

/** Whether @p actual is within @p tolerance of @p wanted , relative to the larger magnitude and
 *  never below one. */
inline bool near_(double actual, double wanted, double tolerance) noexcept {
    double const scale = std::fmax(1.0, std::fmax(std::fabs(actual), std::fabs(wanted)));
    return std::fabs(actual - wanted) <= tolerance * scale;
}

/** Holds when @p actual is near @p wanted ; otherwise counts a failed check and prints both. */
inline bool expect_near(double actual, double wanted, double tolerance,
                        std::source_location where = std::source_location::current()) noexcept {
    if (near_(actual, wanted, tolerance)) return true;
    record_failed_check(where);
    fmt::println(stderr, "    expected {} within {}, got {}", wanted, tolerance, actual);
    return false;
}

/** The library's own spelling of @p code , as @c ustore_status_name answers it, for suites that
 *  link no library. */
template <typename code_type_>
    requires std::is_enum_v<code_type_>
[[nodiscard]] constexpr char const* status_name(code_type_ code) noexcept {
    switch (static_cast<ustore_status_t>(code)) {
    case ustore_status_success_k: return "success";
    case ustore_status_deferred_k: return "deferred";
    case ustore_status_out_of_memory_k: return "out-of-memory";
    case ustore_status_read_only_k: return "read-only";
    case ustore_status_io_k: return "io";
    case ustore_status_device_k: return "device";
    case ustore_status_malformed_k: return "malformed";
    case ustore_status_internal_k: return "internal";
    case ustore_status_uninitialized_k: return "uninitialized";
    case ustore_status_contended_k: return "contended";
    case ustore_status_cancelled_k: return "cancelled";
    case ustore_status_unsupported_k: return "unsupported";
    case ustore_status_invalid_argument_k: return "invalid-argument";
    case ustore_status_not_found_k: return "not-found";
    case ustore_status_already_exists_k: return "already-exists";
    case ustore_status_write_conflict_k: return "write-conflict";
    case ustore_status_read_conflict_k: return "read-conflict";
    case ustore_status_phantom_conflict_k: return "phantom-conflict";
    case ustore_status_capacity_exhausted_k: return "capacity-exhausted";
    }
    return "unrecognized";
}

/** Holds when @p status holds @p wanted ; otherwise counts a failed check and prints both by
 *  name. */
template <typename status_type_, typename code_type_>
    requires std::is_enum_v<code_type_> && requires(status_type_ const& status) {
        { static_cast<ustore_status_t>(status.code) };
        { status.note } -> std::convertible_to<char const*>;
    }
bool expect_status(status_type_ const& status, code_type_ wanted,
                   std::source_location where = std::source_location::current()) noexcept {
    // The engine and the SDK spell the codes as two enums over the same C statuses
    if (static_cast<ustore_status_t>(status.code) == static_cast<ustore_status_t>(wanted)) return true;
    record_failed_check(where);
    fmt::println(stderr, "    expected {}, got {}{}{}", status_name(wanted), status_name(status.code),
                 status.note ? ": " : "", status.note ? status.note : "");
    return false;
}

/** The same check on a bare status code, which a container's refusal answers. */
template <typename code_type_>
    requires std::is_enum_v<code_type_>
bool expect_status(code_type_ code, code_type_ wanted,
                   std::source_location where = std::source_location::current()) noexcept {
    if (code == wanted) return true;
    record_failed_check(where);
    fmt::println(stderr, "    expected {}, got {}", status_name(wanted), status_name(code));
    return false;
}

/** The same check on the status an @c expected holds, so a refusal is checked without unpacking
 *  it. */
template <typename expected_type_, typename code_type_>
    requires requires(expected_type_ const& actual) { actual.status().code; }
bool expect_status(expected_type_ const& actual, code_type_ wanted,
                   std::source_location where = std::source_location::current()) noexcept {
    return expect_status(actual.status(), wanted, where);
}

/** The same check on the status the SDK's aggregate @c expected carries beside its value. */
template <typename expected_type_, typename code_type_>
    requires requires(expected_type_ const& actual) { actual.status.code; }
bool expect_status(expected_type_ const& actual, code_type_ wanted,
                   std::source_location where = std::source_location::current()) noexcept {
    return expect_status(actual.status, wanted, where);
}

/** The status a C call's outcome carries, in the engine's types or in the two the SDK passes. */
template <typename status_type_ = util::status_t, typename code_type_ = util::status_code_t>
status_type_ status_of(ustore_outcome_t const& outcome) noexcept {
    return {static_cast<code_type_>(outcome.status), outcome.message};
}

/** The same check on the outcome of a C call, which it clears either way, so the next call starts
 *  on a blank one. */
template <typename code_type_>
bool expect_status(ustore_outcome_t& outcome, code_type_ wanted,
                   std::source_location where = std::source_location::current()) noexcept {
    bool const held = expect_status(status_of(outcome), wanted, where);
    outcome = {};
    return held;
}

/** Whether a capability probe answered yes, where a probe that failed reads as no. */
template <typename probe_type_>
bool probed(probe_type_ const& probe) noexcept {
    return !probe.status.failed() && probe.value;
}

/** The @c success_k shorthand of @c expect_status on the outcome of a C call. */
inline bool expect_success(ustore_outcome_t& outcome,
                           std::source_location where = std::source_location::current()) noexcept {
    return expect_status(outcome, util::status_code_t::success_k, where);
}

/**
 *  @brief Bisects a memory budget between zero and @p ceiling, running @p body at every rung.
 *
 *  A rung refuses only with @c out_of_memory_k, and the caller checks that nothing stays charged.
 */
template <typename body_type_>
void under_bisected_budget(std::size_t ceiling, body_type_&& body) {
    std::size_t refused = 0;
    std::size_t admitted = ceiling;
    while (refused < admitted) {
        std::size_t const budget = refused + (admitted - refused) / 2;
        util::status_t const status = body(budget);
        if (!status.failed()) {
            admitted = budget;
            continue;
        }
        expect_true(status.code == util::status_code_t::out_of_memory_k, "a budget of {} bytes failed with {}{}{}",
                    budget, status_name(status.code), status.note ? ": " : "", status.note ? status.note : "");
        refused = budget + 1;
    }
}

/** Fills @p tape with @p texts in order, as a deferred call owns its strings, values or fields. */
template <typename tape_type_>
bool fill_tape(tape_type_& tape, std::initializer_list<sz::string_view_t> texts) {
    auto gathered = sz::strings_view_t({texts.begin(), texts.size()}).gather();
    if (gathered.status == sz::status_t::success_k) tape = std::move(gathered.value);
    return expect_true(gathered.status == sz::status_t::success_k);
}

/** The reason a case that needs a filesystem reports when the suite ran without a directory. */
constexpr char const* needs_directory_k = "needs USTORE_DIRECTORY";

/** The reason a case reports on an engine that keeps no transactions. */
constexpr char const* needs_transactions_k = "needs ustore_capabilities_transactions_k";

/** The reason a case reports on an engine that keeps no snapshots. */
constexpr char const* needs_snapshots_k = "needs ustore_capabilities_snapshots_k";

/** The reason a case reports on an engine that serves no pages. */
constexpr char const* needs_pages_k = "needs ustore_pages_read";

/** The reason a device case reports on a machine without one. */
constexpr char const* no_gpu_device_k = "no GPU device";

/** A label in call notation for the suites CTest runs once per engine: the case @p function called
 *  with the exact constant of the engine this run opens, then @p arguments as written, as in the
 *  label <tt>test_database_scan(ustore_engine_ram_k)</tt>. */
struct engine_label_t {

    /** The null-terminated label. */
    char text[192] {};

    explicit engine_label_t(char const* function, char const* arguments = nullptr) noexcept {
        std::string_view const engine = test_run.settings.engine.name;
        constexpr std::pair<std::string_view, char const*> constants_k[] {
            {"ram", "ustore_engine_ram_k"},
            {"rocksdb", "ustore_engine_rocksdb_k"},
            {"leveldb", "ustore_engine_leveldb_k"},
            {"nvme", "ustore_engine_nvme_k"},
        };
        char const* const separator = arguments ? ", " : "";
        if (!arguments) arguments = "";
        auto written = fmt::format_to_n(text, sizeof(text) - 1, "{}(\"{}\"{}{})", function, engine, separator,
                                        arguments);
        for (auto const& [name, constant] : constants_k)
            if (name == engine)
                written = fmt::format_to_n(text, sizeof(text) - 1, "{}({}{}{})", function, constant, separator,
                                           arguments);
        *written.out = '\0';
    }

    /** The label as a C string. */
    [[nodiscard]] char const* c_str() const noexcept { return text; }
};

#pragma region Directories

/** The engine a case opens, with the directory it works in. */
class config_t {
  public:

    /** Opens the engine @p options name in @p directory, or in memory when @p directory is null. */
    explicit config_t(engine_options_t options, char const* directory) noexcept : options_(options) {
        options_.locate(directory);
    }

    /** The engine and the options of its init call, which a second database of a case repeats. */
    [[nodiscard]] engine_options_t const& options() const noexcept { return options_; }

    /** The engine the configuration opens. */
    [[nodiscard]] std::string_view engine() const noexcept { return options_.name; }

    /** The directory the engine works in, or null without a directory. */
    [[nodiscard]] char const* directory() const noexcept { return options_.directory; }

  private:
    engine_options_t options_;
};

/** What a save of @p config's engine names to land in @p path: the manifest NVMe opens there, or
 *  @p path itself. */
inline std::string save_target(config_t const& config, std::string const& path) {
    if (config.engine() != "nvme") return path;
    return fmt::format("{}/ustore.ustore.json", path);
}

/** Opens the engine @p options name into @p database on an executor of one thread spawned for the
 *  open and freed after it, which holds the transport of @c nvme, as the cases run their own
 *  calls on the calling thread. */
inline void open_on_own_executor(engine_options_t options, ustore_database_t* database,
                                 ustore_outcome_t* outcome) noexcept {
    options.threads.value = 1;
    ustore_executor_t executor = nullptr;
    spawn_executor(options, &executor, outcome);
    if (outcome->status == ustore_status_success_k) open_engine(options, executor, database, outcome);
    ustore_executor_free(executor);
}

/** Opens the engine @p config names through the C ABI. */
inline util::expected<ustore_database_t> open_raw(config_t const& config) noexcept {
    ustore_database_t database = nullptr;
    ustore_outcome_t outcome {};
    open_on_own_executor(config.options(), &database, &outcome);
    if (util::status_t const status = status_of(outcome); status.failed()) return status;
    return database;
}

/** Opens the engine @p config names as a C++ @p database_type_, which adopts the handle. A template, so this header
 *  needs no @c ustore/db.hpp, whose names would clash with the storage suites' own. */
template <typename database_type_>
util::expected<database_type_> open_database(config_t const& config) noexcept {
    auto [database, open_status] = open_raw(config);
    if (open_status.failed()) return open_status;
    return database_type_ {database};
}

/** Creates @p directory with its missing parents, answering whether it exists. */
[[nodiscard]] inline bool create_directories(char const* directory) noexcept {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return !error;
}

/**
 *  @brief A unique directory under the parent the @c DIRECTORY setting names, removed with its
 *      contents on destruction.
 *
 *  Without a parent it names no path, and the cases that need persistence report skipped.
 */
class scratch_directory_t {
  public:
    /** Names no directory, as a moved-from one does. */
    scratch_directory_t() noexcept = default;

    scratch_directory_t(scratch_directory_t&& other) noexcept {
        std::memcpy(path_, other.path_, sizeof(path_));
        other.path_[0] = '\0';
    }

    scratch_directory_t& operator=(scratch_directory_t&&) = delete;
    scratch_directory_t(scratch_directory_t const&) = delete;
    scratch_directory_t& operator=(scratch_directory_t const&) = delete;

    ~scratch_directory_t() noexcept {
        if (path_[0] == '\0') return;
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        if (error) fmt::println(stderr, "Failed to remove {}: {}", path_, error.message());
    }

    /** Creates @p parent with its parents and a unique directory inside it, or names none for an empty @p parent. */
    [[nodiscard]] static st::expected<scratch_directory_t> make(std::string_view parent) noexcept {
        using st::status_t;
        scratch_directory_t directory;
        if (parent.empty()) return directory;

        std::error_code error;
        std::filesystem::path const parent_path {parent};
        std::filesystem::create_directories(parent_path, error);
        if (error) return status_t::input_output_k;

        // A name another suite already took answers false rather than an error, so the next tick is tried
        auto const tick = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        for (std::uint64_t attempt = 0; attempt != 64; ++attempt) {
            char name[32];
            *fmt::format_to_n(name, sizeof(name) - 1, "ustore-{:016x}", tick + attempt).out = '\0';
            std::filesystem::path const candidate = parent_path / name;
            if (!std::filesystem::create_directory(candidate, error)) {
                if (error) return status_t::input_output_k;
                continue;
            }
            // Forward slashes serve every platform and need no escaping inside the JSON configuration
            std::string const text = candidate.generic_string();
            if (text.size() >= path_capacity_k) return status_t::invalid_argument_k;
            std::memcpy(directory.path_, text.c_str(), text.size() + 1);
            return directory;
        }
        return status_t::key_already_exists_k;
    }

    /** Removes @p directory with its contents and creates it again with its parents, answering
     *  whether it exists. */
    [[nodiscard]] static bool recreate(char const* directory) noexcept {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
        return !error && create_directories(directory);
    }

    /** The unique directory, or null without a parent. */
    [[nodiscard]] char const* path() const noexcept { return path_[0] != '\0' ? path_ : nullptr; }

  private:
    /** Longest path the unique directory takes. */
    static constexpr std::size_t path_capacity_k = 4096;

    /** The null-terminated path, empty when the suite runs without a directory. */
    char path_[path_capacity_k] {};
};

#pragma endregion Directories

} // namespace unum::ustore::test
