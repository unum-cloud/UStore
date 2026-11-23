/**
 *  @file bench/harness.hpp
 *  @author Ash Vardanian
 *  @date September 30, 2026
 *  @brief Timed loops, latency histograms, result rows and the flags every benchmark shares.
 *
 *  A row runs one body on every thread of an @c executor_t, each thread iterating its own @c loop_t
 *  in a range-for, and folds the loops into one @c row_t. Setup above the range-for stays untimed.
 *  Every call lands in its thread's latency histogram, so p50, p99 and p99.9 cost no shared writes.
 *
 *  Every benchmark takes these settings beside its own, each a <tt>--flag</tt> overriding its
 *  <tt>USTORE_</tt> variable, and echoes them all as "- Name: value" before its rows:
 *
 *  @verbatim
 *  Setting       Default  Meaning
 *  --warmup      1s       Untimed calls ahead of each sampled row, like 200ms
 *  --time-limit  10s      Timed calls of each sampled row, like 10s
 *  --seed        42       Seed of generated data and key streams, or random
 *  --filter      none     Rows to run, by ECMAScript regex, or else by substring
 *  @endverbatim
 */
#pragma once

#include <cmath>   // `std::ceil`, `std::isfinite`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`
#include <cstdio>  // `std::FILE`
#include <cstdlib> // `std::exit`, `std::getenv`, `std::strtod`
#include <cstring> // `std::strcmp`, `std::strncmp`

#include <array>        // `std::array`
#include <bit>          // `std::bit_width`
#include <charconv>     // `std::from_chars`
#include <chrono>       // `std::chrono::steady_clock`
#include <limits>       // `std::numeric_limits`
#include <random>       // `std::random_device`
#include <regex>        // `std::regex`
#include <span>         // `std::span`
#include <string>       // `std::string`
#include <string_view>  // `std::string_view`
#include <system_error> // `std::errc`
#include <thread>       // `std::thread::hardware_concurrency`

#include <fmt/format.h>

#include <smashtable/basic_vector.hpp>

#include "ustore/utilities/ram_device.hpp" // `executor_t`

#include "arguments.hpp"

namespace unum::ustore::bench {

using namespace tools;

using steady_clock_t = std::chrono::steady_clock;
using time_point_t = steady_clock_t::time_point;

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

#pragma region Settings

/** The settings every benchmark shares, which each benchmark's own settings extend. */
struct bench_settings_t : common_settings_t {

    /** Untimed calls ahead of each sampled row. */
    std::chrono::milliseconds warmup = std::chrono::seconds {1};

    /** Timed calls of each sampled row. */
    std::chrono::milliseconds time_limit = std::chrono::seconds {10};
};

/**
 *  @brief Prints the version line, then reads the shared settings into @p settings and every one
 *      of @p flags from its flag or the environment variable @p variables names beside it, and
 *      echoes them all.
 *
 *  A value that does not parse prints one line naming its variable and exits with status 1.
 */
template <std::size_t count_>
void read_bench_settings(int argc, char** argv, bench_settings_t& settings, std::array<flag_t, count_> const& flags,
                         std::array<char const*, count_> const& variables) {
    fmt::println("UStore {}.{}.{}", USTORE_VERSION_MAJOR, USTORE_VERSION_MINOR, USTORE_VERSION_PATCH);
    fmt::println("- This machine: {} threads", std::thread::hardware_concurrency());
    settings.seed = read_seed_setting(argc, argv, "USTORE_SEED", "--seed");
    settings.regex_filter = read_regex_filter_setting(argc, argv, "USTORE_FILTER", "--filter");
    settings.program = argc != 0 ? argv[0] : "";
    settings.warmup = read_duration_setting(argc, argv, "USTORE_WARMUP", "--warmup", settings.warmup);
    settings.time_limit = read_duration_setting(argc, argv, "USTORE_TIME_LIMIT", "--time-limit", settings.time_limit);
    for (std::size_t index = 0; index != count_; ++index) {
        char const* text = setting_text(argc, argv, variables[index], flags[index].name);
        if (text && assign_variable_(flags[index], text) != argument_status_t::parsed_k)
            exit_unparsed(variables[index], text, grammar(flags[index]).data());
    }
    print_flags(flags);
    fmt::println("- Warmup: {}ms", settings.warmup.count());
    fmt::println("- Time limit: {}ms", settings.time_limit.count());
    fmt::println("- Seed: {}", settings.seed);
    fmt::println("- Filter: {}",
                 settings.regex_filter.text.empty() ? std::string_view("none") : settings.regex_filter.text);
}

#pragma endregion Settings

#pragma region Loops

/** Call latencies in nanoseconds, 16 linear bins per power of two, so a percentile reads at most 1/16 high. */
class latency_histogram_t {
  public:

    /** Counts one call that took @p duration. */
    void record(steady_clock_t::duration duration) noexcept {
        ++bins_[bin_of_(static_cast<std::uint64_t>(std::chrono::nanoseconds(duration).count()))];
    }

    /** Adds every count of @p other, folding one thread's histogram into the row's. */
    void merge(latency_histogram_t const& other) noexcept {
        for (std::size_t bin = 0; bin != bins_k; ++bin) bins_[bin] += other.bins_[bin];
    }

    /** The upper edge of the bin holding the nearest-rank @p fraction, like @c 0.999 for p99.9, or zero when empty. */
    [[nodiscard]] std::chrono::nanoseconds percentile(double fraction) const noexcept {
        std::uint64_t total = 0;
        for (std::uint64_t const count : bins_) total += count;
        if (total == 0) return {};
        std::uint64_t const rank = fu::max_of<std::uint64_t>(
            1, static_cast<std::uint64_t>(std::ceil(fraction * static_cast<double>(total))));
        std::uint64_t cumulative = 0;
        std::size_t bin = 0;
        while (bin + 1 != bins_k && (cumulative += bins_[bin]) < rank) ++bin;
        return std::chrono::nanoseconds(static_cast<std::chrono::nanoseconds::rep>(upper_edge_(bin)));
    }

  private:
    static constexpr std::size_t sub_bins_k = 16;

    /** One exact bin per value below 32, then 16 per power of two up to the 64th bit. */
    static constexpr std::size_t bins_k = 61 * sub_bins_k;

    static constexpr std::size_t bin_of_(std::uint64_t nanoseconds) noexcept {
        if (nanoseconds < 2 * sub_bins_k) return static_cast<std::size_t>(nanoseconds);
        std::size_t const shift = static_cast<std::size_t>(std::bit_width(nanoseconds)) - 5;
        return shift * sub_bins_k + static_cast<std::size_t>(nanoseconds >> shift);
    }

    static constexpr std::uint64_t upper_edge_(std::size_t bin) noexcept {
        if (bin < 2 * sub_bins_k) return bin;
        std::size_t const shift = bin / sub_bins_k - 1;
        std::uint64_t const leading = bin % sub_bins_k + sub_bins_k;
        return ((leading + 1) << shift) - 1;
    }

    /** Calls per bin. */
    std::array<std::uint64_t, bins_k> bins_ {};
};

/** A finished row, folded from every thread's loop: its counts and latencies, or why it was skipped. */
struct row_t {
    std::string_view name;
    std::size_t threads = 0;
    std::uint64_t calls = 0, items = 0, bytes = 0, failures = 0;
    double seconds = 0;
    std::chrono::nanoseconds p50 {}, p99 {}, p999 {};
    char const* skipped = nullptr;
};

/**
 *  @brief One thread's timed loop, iterated as `for (std::size_t call : loop)`.
 *
 *  A sampled loop runs untimed until its warm-up ends, then times calls until its time limit. A pass visits each call
 *  of its range once and times every one. The loop reads the clock between calls, so each call's latency lands in the
 *  histogram, and the body reports what a call did through @c count, @c fail and @c skip.
 */
class loop_t {
    time_point_t warmup_until_ = (time_point_t::min)();
    time_point_t deadline_ = (time_point_t::max)();
    time_point_t started_ {}, previous_ {};
    std::size_t index_ = 0, end_ = (std::numeric_limits<std::size_t>::max)();
    std::uint64_t calls_ = 0, items_ = 0, bytes_ = 0, failures_ = 0;
    steady_clock_t::duration elapsed_ {};
    bool warming_up_ = false;
    char const* skipped_ = nullptr;
    latency_histogram_t latencies_;

    bool keep_running_() noexcept {
        time_point_t const now = steady_clock_t::now();
        if (warming_up_ && now >= warmup_until_)
            warming_up_ = false, started_ = now, calls_ = 0, items_ = 0, bytes_ = 0, failures_ = 0;
        else if (!warming_up_ && calls_ != 0) latencies_.record(now - previous_);
        previous_ = now;
        if (!warming_up_) elapsed_ = now - started_;
        return !skipped_ && index_ != end_ && (warming_up_ || now < deadline_);
    }

  public:
    struct end_t {};
    struct iterator_t {
        loop_t* loop;
        bool operator!=(end_t) noexcept { return loop->keep_running_(); }
        std::size_t operator*() const noexcept { return loop->index_; }
        void operator++() noexcept { ++loop->index_, ++loop->calls_; }
    };

    loop_t() noexcept = default;

    /** A sampled loop, untimed until @p warmup_until and timed until @p deadline. */
    loop_t(time_point_t warmup_until, time_point_t deadline) noexcept
        : warmup_until_(warmup_until), deadline_(deadline) {}

    /** A pass over the calls @b [first,last), each timed once. */
    loop_t(std::size_t first, std::size_t last) noexcept : index_(first), end_(last) {}

    iterator_t begin() noexcept {
        started_ = previous_ = steady_clock_t::now();
        warming_up_ = started_ < warmup_until_;
        return {this};
    }
    end_t end() const noexcept { return {}; }

    /** Counts the @p items keys, documents or edges and the @p bytes the current call handled. */
    void count(std::size_t items, std::size_t bytes = 0) noexcept { items_ += items, bytes_ += bytes; }

    /** Counts the current call as failed. */
    void fail() noexcept { ++failures_; }

    /** Stops the loop and reports @p reason instead of results. */
    void skip(char const* reason) noexcept { skipped_ = reason; }

    /** Adds this loop's counts and latencies to @p row and @p latencies. */
    void fold_into(row_t& row, latency_histogram_t& latencies) const noexcept {
        row.calls += calls_, row.items += items_, row.bytes += bytes_, row.failures += failures_;
        row.seconds = fu::max_of(row.seconds, std::chrono::duration<double>(elapsed_).count());
        if (skipped_) row.skipped = skipped_;
        latencies.merge(latencies_);
    }
};

/** Folds every loop of @p loops into the row named @p name. */
[[nodiscard]] inline row_t fold(std::string_view name, std::span<loop_t const> loops) noexcept {
    row_t row {.name = name, .threads = loops.size()};
    latency_histogram_t latencies;
    for (loop_t const& loop : loops) loop.fold_into(row, latencies);
    row.p50 = latencies.percentile(0.5), row.p99 = latencies.percentile(0.99), row.p999 = latencies.percentile(0.999);
    return row;
}

/**
 *  @brief Samples @p body on every thread of @p executor: each warms up for @c --warmup, then runs
 *      for @c --time-limit, the deadlines shared by all threads.
 *  @param[in] body Called as `body(loop, thread)` on each thread; setup above its loop stays untimed.
 */
template <typename body_type_>
[[nodiscard]] row_t run_threads(util::executor_t& executor, bench_settings_t const& settings, std::string_view name,
                                body_type_&& body) noexcept {
    st::vector<loop_t> loops;
    if (st::failed(loops.resize(executor.threads_count()))) return {.name = name, .skipped = "no memory for loops"};
    time_point_t const warmup_until = steady_clock_t::now() + settings.warmup;
    for (loop_t& loop : loops) loop = loop_t {warmup_until, warmup_until + settings.time_limit};
    if (st::failed(executor.for_threads([&](std::size_t thread) noexcept { body(loops[thread], thread); })))
        return {.name = name, .skipped = "the pool refused the dispatch"};
    return fold(name, loops);
}

/**
 *  @brief Runs @p body over the calls @b [0,count) once, thread @c t of @p executor taking the
 *      contiguous share @b [count*t/threads,count*(t+1)/threads), for input a row consumes, like
 *      building a collection.
 *  @param[in] body Called as `body(loop, thread)` on each thread; setup above its loop stays untimed.
 */
template <typename body_type_>
[[nodiscard]] row_t run_pass(util::executor_t& executor, std::string_view name, std::size_t count,
                             body_type_&& body) noexcept {
    st::vector<loop_t> loops;
    std::size_t const threads = executor.threads_count();
    if (st::failed(loops.resize(threads))) return {.name = name, .skipped = "no memory for loops"};
    for (std::size_t thread = 0; thread != threads; ++thread)
        loops[thread] = loop_t {count * thread / threads, count * (thread + 1) / threads};
    if (st::failed(executor.for_threads([&](std::size_t thread) noexcept { body(loops[thread], thread); })))
        return {.name = name, .skipped = "the pool refused the dispatch"};
    return fold(name, loops);
}

#pragma endregion Loops

#pragma region Output

/** Spells @p per_second in decimal units, like "789" or "2.61 M", so the unit that follows takes one space. */
[[nodiscard]] inline std::string spell_decimal_rate(double per_second) {
    char const* prefix = "";
    for (char const* next : {"k", "M", "G", "T"})
        if (per_second >= 1000) per_second /= 1000, prefix = next;
    return *prefix ? fmt::format("{:.2f} {}", per_second, prefix) : fmt::format("{:.0f}", per_second);
}

/** Spells @p per_second bytes in binary units, like "789 B/s" or "412.00 MB/s". */
[[nodiscard]] inline std::string spell_byte_rate(double per_second) {
    char const* unit = "B/s";
    for (char const* next : {"KB/s", "MB/s", "GB/s", "TB/s"})
        if (per_second >= 1024) per_second /= 1024, unit = next;
    return *unit == 'B' ? fmt::format("{:.0f} {}", per_second, unit) : fmt::format("{:.2f} {}", per_second, unit);
}

/** Spells @p latency in the largest unit under it, like "850 ns", "12.1 us" or "1.52 s". */
[[nodiscard]] inline std::string spell_latency(std::chrono::nanoseconds latency) {
    double const nanoseconds = static_cast<double>(latency.count());
    if (nanoseconds < 1e3) return fmt::format("{:.0f} ns", nanoseconds);
    if (nanoseconds < 1e6) return fmt::format("{:.1f} us", nanoseconds / 1e3);
    if (nanoseconds < 1e9) return fmt::format("{:.1f} ms", nanoseconds / 1e6);
    return fmt::format("{:.2f} s", nanoseconds / 1e9);
}

/** Prints @p row on one line: items per second in decimal units, bytes in binary ones, then the latencies. */
inline void print(row_t const& row) {
    if (row.skipped) return fmt::println("{:<36} skipped: {}", row.name, row.skipped);
    double const seconds = row.seconds > 0 ? row.seconds : std::numeric_limits<double>::infinity();
    fmt::println("{:<36} {:>3} threads  {} items/s  {}  p50 {}  p99 {}  p99.9 {}  {} failures", row.name, row.threads,
                 spell_decimal_rate(static_cast<double>(row.items) / seconds),
                 spell_byte_rate(static_cast<double>(row.bytes) / seconds), spell_latency(row.p50),
                 spell_latency(row.p99), spell_latency(row.p999), row.failures);
}

#pragma endregion Output

} // namespace unum::ustore::bench
