/**
 *  @file tools/arguments.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief Command-line flags for every UStore binary: one table parses the arguments, reads their
 *      variables, prints the usage and echoes the values.
 *
 *  Flags are long kebab-case words whose value follows as the next argument or after @c =. Byte
 *  counts hold a binary suffix, durations hold @c ms or @c s, counts are plain integers, and
 *  lists are comma-separated. A text value views the tail of its @c argv entry, so it is
 *  null-terminated; list items are not. A value that does not parse prints @c --flag="value"
 *  @c does @c not @c parse, @c expected @c <grammar>.
 *
 *  @code{.cpp}
 *  bytes_t memory_limit;
 *  std::string_view directory;
 *  std::array<std::string_view, 2> const formats {"ndjson", "csv"};
 *  choice_t format {formats};
 *  std::array<flag_t, 3> const flags {{
 *      {"--directory", "path", "database directory", &directory},
 *      {"--memory-limit", "bytes", "bytes a batch buffers, like 64M", &memory_limit},
 *      {"--format", "name", "export format: ndjson or csv", &format},
 *  }};
 *  arguments_t const parsed = parse_arguments(argc, argv, flags, {});
 *  @endcode
 */
#pragma once

#include <cctype> // `std::toupper`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`
#include <cstdio>  // `stderr`
#include <cstdlib> // `std::exit`, `std::strtod`, `std::getenv`
#include <cstring> // `std::strlen`

#include "ustore/types.h" // `USTORE_OS_LINUX_`

#if USTORE_OS_LINUX_
#include <sched.h> // `sched_getaffinity`, `CPU_COUNT`
#endif

#include <array>        // `std::array`
#include <charconv>     // `std::from_chars`
#include <chrono>       // `std::chrono::milliseconds`
#include <limits>       // `std::numeric_limits`
#include <optional>     // `std::optional`
#include <random>       // `std::random_device`
#include <regex>        // `std::regex`, `std::regex_search` - the filter
#include <span>         // `std::span`
#include <string_view>  // `std::string_view`
#include <system_error> // `std::errc`
#include <thread>       // `std::thread::hardware_concurrency`
#include <utility>      // `std::forward`
#include <variant>      // `std::variant`

#include <fmt/chrono.h> // `std::chrono::milliseconds` through `{}`
#include <fmt/format.h>
#include <fmt/ranges.h> // `fmt::join`

#include <stringzilla/string.hpp> // `sz::string_t`

#include "ustore/db.h"                         // the init call of every engine and the spellings flags name
#include "ustore/utilities/blocking_files.hpp" // `read_file`, `path_max_bytes_k`
#include "ustore/utilities/nvme_volume.hpp"    // `parse_data_address`, `manifest_suffix_k`
#include "ustore/utilities/types.hpp"          // `sz`

/** Prints an owning StringZilla string as the text it holds, never as a range of characters. */
template <>
struct fmt::formatter<ashvardanian::stringzilla::string_t> : fmt::formatter<std::string_view> {
    fmt::format_context::iterator format(ashvardanian::stringzilla::string_t const& text,
                                         fmt::format_context& context) const {
        return fmt::formatter<std::string_view>::format(text, context);
    }
};

namespace unum::ustore::tools {

/** Exits with status 1 after printing that @p what could not be allocated. */
[[noreturn]] inline void exit_out_of_memory(std::string_view what) noexcept {
    fmt::println(stderr, "out of memory for {}", what);
    std::exit(1);
}

/** Appends @p format over @p args to @p text, exiting with status 1 when it cannot grow. */
template <typename... args_types_>
void append_formatted(sz::string_t& text, fmt::format_string<args_types_ const&...> format,
                      args_types_ const&... args) noexcept {
    std::size_t const length = text.size();
    if (text.resize(length + fmt::formatted_size(format, args...)) != sz::status_t::success_k)
        exit_out_of_memory("a text");
    fmt::format_to(text.data() + length, format, args...);
}

/** @p format over @p args as an owning string, exiting with status 1 when it cannot allocate. */
template <typename... args_types_>
sz::string_t formatted(fmt::format_string<args_types_ const&...> format, args_types_ const&... args) noexcept {
    sz::string_t text;
    append_formatted(text, format, args...);
    return text;
}

/** Outcome of parsing one value or a whole argument list. */
enum class argument_status_t {

    /** Every argument matched its flag. */
    parsed_k,

    /** <tt>--help</tt> was given, so the caller prints the usage and exits. */
    help_k,

    /** An argument starting with <tt>--</tt> names no flag. */
    unknown_flag_k,

    /** A flag taking a value ended the argument list. */
    missing_value_k,

    /** A switch was given a value through @c =. */
    unexpected_value_k,

    /** The value does not parse: no non-negative integer, duration, real or ECMAScript regex. */
    invalid_number_k,

    /** The value overflows its destination or falls outside its bounds. */
    out_of_range_k,

    /** A list or the positional arguments exceed their capacity. */
    too_many_items_k,

    /** A list holds fewer items than it requires. */
    too_few_items_k,

    /** The value names no enumerator of its destination. */
    unknown_name_k,
};

/** A plain non-negative integer, accepted only within @b [minimum,maximum]. */
struct count_t {
    std::size_t value = 0;
    std::size_t minimum = 1;
    std::size_t maximum = (std::numeric_limits<std::size_t>::max)();
};

/** Comma-separated counts like @c 16,256,4096, each within @b [minimum,maximum].
 *  Between @c minimum_size and @c maximum_size of them are accepted. */
struct counts_t {
    static constexpr std::size_t capacity_k = 16;
    std::array<std::size_t, capacity_k> values {};
    std::size_t size = 0;
    std::size_t minimum = 1;
    std::size_t maximum = (std::numeric_limits<std::size_t>::max)();
    std::size_t minimum_size = 1;
    std::size_t maximum_size = capacity_k;

    [[nodiscard]] std::span<std::size_t const> view() const noexcept { return {values.data(), size}; }
};

/** A byte count: an integer, or one suffixed K, M, G or T for its binary multiple, like 24G. */
struct bytes_t {
    std::size_t value = 0;
};

/** Whether a flag without a value appeared. */
enum class switch_t : bool {
    absent_k,
    present_k,
};

/** Which named checks run: those whose names its ECMAScript regex matches, all while unset. */
struct regex_filter_t {
    std::string_view text;
    std::regex regex {""};

    bool selects(std::string_view name) const noexcept { return std::regex_search(name.begin(), name.end(), regex); }
};

/** Compiles @p text into @p filter, refusing a pattern that is no ECMAScript regex. */
[[nodiscard]] inline argument_status_t filter_of(std::string_view text, regex_filter_t& filter) noexcept {
    filter.text = text;
#if defined(__cpp_exceptions) && __cpp_exceptions
    try {
        filter.regex.assign(text.begin(), text.end());
    }
    catch (std::regex_error const&) {
        return argument_status_t::invalid_number_k;
    }
#else
    filter.regex.assign(text.begin(), text.end());
#endif
    return argument_status_t::parsed_k;
}

/** Comma-separated text items like @c /mnt/a,/mnt/b, viewing into @c argv without null terminators. */
struct texts_t {
    static constexpr std::size_t capacity_k = 16;
    std::array<std::string_view, capacity_k> values {};
    std::size_t size = 0;

    [[nodiscard]] std::span<std::string_view const> view() const noexcept { return {values.data(), size}; }
};

/** One name out of @c names, for a selection the C interface spells nowhere, like a benchmark's
 *  @c cpu|gpu; @c selected indexes the chosen one. */
struct choice_t {
    std::span<std::string_view const> names;
    std::size_t selected = 0;
};

/** One flag: its name with both dashes, the usage placeholder, one help line, and the value it
 *  writes. */
struct flag_t {
    std::string_view name;
    std::string_view placeholder;
    std::string_view help;
    std::variant<std::string_view*, count_t*, counts_t*, switch_t*, std::chrono::milliseconds*, std::uint32_t*, double*,
                 regex_filter_t*, texts_t*, choice_t*, bytes_t*>
        parsed_variant;
};

/** Result of @c parse_arguments: the status, the argument and flag it concerns, the value that
 *  failed, and how many positionals were collected. */
struct arguments_t {
    argument_status_t status = argument_status_t::parsed_k;
    std::string_view argument;
    std::string_view value;
    flag_t const* flag = nullptr;
    std::size_t positionals_count = 0;
};

[[nodiscard]] inline argument_status_t parse_integer_(std::string_view digits, std::uint64_t& value) noexcept {
    if (digits.empty()) return argument_status_t::invalid_number_k;
    auto const [end, parse_status] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (parse_status == std::errc::result_out_of_range) return argument_status_t::out_of_range_k;
    if (parse_status != std::errc {} || end != digits.data() + digits.size())
        return argument_status_t::invalid_number_k;
    return argument_status_t::parsed_k;
}

[[nodiscard]] inline argument_status_t parse_bounded_(std::string_view text, std::size_t minimum, std::size_t maximum,
                                                      std::size_t& value) noexcept {
    std::uint64_t parsed = 0;
    argument_status_t const status = parse_integer_(text, parsed);
    if (status != argument_status_t::parsed_k) return status;
    if (parsed < minimum || parsed > maximum) return argument_status_t::out_of_range_k;
    value = static_cast<std::size_t>(parsed);
    return argument_status_t::parsed_k;
}

/** Parses a plain integer within the bounds @p count already holds. */
[[nodiscard]] inline argument_status_t parse_count(std::string_view text, count_t& count) noexcept {
    return parse_bounded_(text, count.minimum, count.maximum, count.value);
}

/** Parses comma-separated counts within the bounds @p counts holds, replacing the items it
 *  held. */
[[nodiscard]] inline argument_status_t parse_counts(std::string_view text, counts_t& counts) noexcept {
    std::size_t const maximum_size = counts.maximum_size < counts_t::capacity_k ? counts.maximum_size
                                                                                : counts_t::capacity_k;
    counts.size = 0;
    while (true) {
        std::size_t const comma = text.find(',');
        if (counts.size == maximum_size) return argument_status_t::too_many_items_k;
        argument_status_t const status = parse_bounded_(text.substr(0, comma), counts.minimum, counts.maximum,
                                                        counts.values[counts.size]);
        if (status != argument_status_t::parsed_k) return status;
        ++counts.size;
        if (comma == std::string_view::npos) break;
        text.remove_prefix(comma + 1);
    }
    return counts.size < counts.minimum_size ? argument_status_t::too_few_items_k : argument_status_t::parsed_k;
}

/** Parses a byte count into @p bytes: digits, then an optional K, M, G or T. */
[[nodiscard]] inline argument_status_t parse_bytes(std::string_view text, bytes_t& bytes) noexcept {
    std::size_t shift = 0;
    if (!text.empty()) switch (text.back()) {
        case 'K': shift = 10; break;
        case 'M': shift = 20; break;
        case 'G': shift = 30; break;
        case 'T': shift = 40; break;
        default: break;
        }
    if (shift) text.remove_suffix(1);
    std::uint64_t parsed = 0;
    if (argument_status_t const status = parse_integer_(text, parsed); status != argument_status_t::parsed_k)
        return status;
    if (parsed > ((std::numeric_limits<std::size_t>::max)() >> shift)) return argument_status_t::out_of_range_k;
    bytes.value = static_cast<std::size_t>(parsed) << shift;
    return argument_status_t::parsed_k;
}

/** A positive duration in whole milliseconds or seconds, like "200ms" or "10s". */
inline std::optional<std::chrono::milliseconds> parse_duration(std::string_view text) noexcept {
    std::uint32_t count = 0;
    auto const [end, parse_status] = std::from_chars(text.data(), text.data() + text.size(), count);
    if (parse_status != std::errc {} || !count) return std::nullopt;
    std::string_view const unit = text.substr(end - text.data());
    if (unit == "ms") return std::chrono::milliseconds(count);
    if (unit == "s") return std::chrono::seconds(count);
    return std::nullopt;
}

/** A 32-bit seed, or "random" for a fresh draw from @c std::random_device. */
inline std::optional<std::uint32_t> parse_seed(std::string_view text) noexcept {
    if (text == "random") return static_cast<std::uint32_t>(std::random_device {}());
    std::uint32_t seed = 0;
    auto const [end, parse_status] = std::from_chars(text.data(), text.data() + text.size(), seed);
    if (parse_status != std::errc {} || end != text.data() + text.size()) return std::nullopt;
    return seed;
}

/** A non-negative real, like "0.125" or "1e-3". */
inline std::optional<double> parse_real(std::string_view text) noexcept {
    // Text values view the tail of their `argv` entry, so `strtod` stops at its terminator.
    char* end = nullptr;
    double const real = std::strtod(text.data(), &end);
    if (text.empty() || end != text.data() + text.size() || !(real >= 0)) return std::nullopt;
    return real;
}

/** Splits comma-separated text into @p texts, rejecting empty items. */
[[nodiscard]] inline argument_status_t parse_texts(std::string_view text, texts_t& texts) noexcept {
    texts.size = 0;
    while (true) {
        std::size_t const comma = text.find(',');
        if (texts.size == texts_t::capacity_k) return argument_status_t::too_many_items_k;
        std::string_view const item = text.substr(0, comma);
        if (item.empty()) return argument_status_t::too_few_items_k;
        texts.values[texts.size++] = item;
        if (comma == std::string_view::npos) return argument_status_t::parsed_k;
        text.remove_prefix(comma + 1);
    }
}

/** Selects the name in @p choice equal to @p text. */
[[nodiscard]] inline argument_status_t parse_choice(std::string_view text, choice_t& choice) noexcept {
    for (std::size_t index = 0; index != choice.names.size(); ++index) {
        if (choice.names[index] != text) continue;
        choice.selected = index;
        return argument_status_t::parsed_k;
    }
    return argument_status_t::unknown_name_k;
}

/** The bytes of @p text as the C interface views a path or a name. */
inline ustore_bytes_view_t bytes_view(std::string_view text) noexcept {
    return {reinterpret_cast<ustore_byte_t const*>(text.data()), text.size()};
}

/** Writes the enumerator @p value spells into @p destination, read back by @p from_name and
 *  accepted only when @p name spells it the same way; the unknown enumerator at zero is no value a
 *  flag names, since leaving the flag out derives it. */
template <typename enumeration_type_>
[[nodiscard]] argument_status_t assign_named_(std::string_view value, enumeration_type_& destination,
                                              enumeration_type_ (*from_name)(ustore_bytes_view_t),
                                              char const* (*name)(enumeration_type_)) noexcept {
    enumeration_type_ const parsed = from_name(bytes_view(value));
    if (parsed == enumeration_type_ {} || value != name(parsed)) return argument_status_t::unknown_name_k;
    destination = parsed;
    return argument_status_t::parsed_k;
}

/** Writes the enumerator a status-answering @p from_name reads @p value as into @p destination. */
template <typename enumeration_type_>
[[nodiscard]] argument_status_t assign_named_(std::string_view value, enumeration_type_& destination,
                                              ustore_status_t (*from_name)(ustore_bytes_view_t,
                                                                           enumeration_type_*)) noexcept {
    enumeration_type_ parsed {};
    if (from_name(bytes_view(value), &parsed) != ustore_status_success_k) return argument_status_t::unknown_name_k;
    destination = parsed;
    return argument_status_t::parsed_k;
}

[[nodiscard]] inline argument_status_t assign_(flag_t const& flag, std::string_view value) noexcept {
    if (std::string_view* const* text = std::get_if<std::string_view*>(&flag.parsed_variant)) {
        **text = value;
        return argument_status_t::parsed_k;
    }
    if (count_t* const* count = std::get_if<count_t*>(&flag.parsed_variant)) return parse_count(value, **count);
    if (counts_t* const* counts = std::get_if<counts_t*>(&flag.parsed_variant)) return parse_counts(value, **counts);
    if (std::chrono::milliseconds* const* duration = std::get_if<std::chrono::milliseconds*>(&flag.parsed_variant)) {
        std::optional<std::chrono::milliseconds> const parsed = parse_duration(value);
        if (!parsed) return argument_status_t::invalid_number_k;
        **duration = *parsed;
        return argument_status_t::parsed_k;
    }
    if (std::uint32_t* const* seed = std::get_if<std::uint32_t*>(&flag.parsed_variant)) {
        std::optional<std::uint32_t> const parsed = parse_seed(value);
        if (!parsed) return argument_status_t::invalid_number_k;
        **seed = *parsed;
        return argument_status_t::parsed_k;
    }
    if (double* const* real = std::get_if<double*>(&flag.parsed_variant)) {
        std::optional<double> const parsed = parse_real(value);
        if (!parsed) return argument_status_t::invalid_number_k;
        **real = *parsed;
        return argument_status_t::parsed_k;
    }
    if (regex_filter_t* const* filter = std::get_if<regex_filter_t*>(&flag.parsed_variant))
        return filter_of(value, **filter);
    if (texts_t* const* texts = std::get_if<texts_t*>(&flag.parsed_variant)) return parse_texts(value, **texts);
    if (choice_t* const* choice = std::get_if<choice_t*>(&flag.parsed_variant)) return parse_choice(value, **choice);
    if (bytes_t* const* bytes = std::get_if<bytes_t*>(&flag.parsed_variant)) return parse_bytes(value, **bytes);
    return argument_status_t::unexpected_value_k;
}

/**
 *  @brief Parses @p argv against @p flags, collecting undashed arguments into @p positionals.
 *
 *  Parsing stops at the first failure, leaving earlier destinations written.
 *
 *  @param[in] argc Entries of @p argv, the first of which is the program name and is skipped.
 *  @param[in] argv The arguments, which every text destination and list item goes on viewing.
 *  @param[in] flags Every flag the binary accepts; @c --help is always accepted and never listed.
 *  @param[out] positionals Receives positional arguments in order, viewing into @p argv.
 */
[[nodiscard]] inline arguments_t parse_arguments(int argc, char const* const* argv, std::span<flag_t const> flags,
                                                 std::span<std::string_view> positionals) noexcept {
    arguments_t result;
    for (int index = 1; index < argc; ++index) {
        std::string_view const argument = argv[index];
        result.argument = argument, result.value = {}, result.flag = nullptr;

        if (!argument.starts_with("--")) {
            if (result.positionals_count == positionals.size()) {
                result.status = argument_status_t::too_many_items_k;
                return result;
            }
            positionals[result.positionals_count++] = argument;
            continue;
        }
        if (argument == "--help") {
            result.status = argument_status_t::help_k;
            return result;
        }

        std::size_t const equals = argument.find('=');
        std::string_view const name = argument.substr(0, equals);
        flag_t const* flag = nullptr;
        for (flag_t const& candidate : flags)
            if (candidate.name == name) flag = &candidate;
        if (!flag) {
            result.status = argument_status_t::unknown_flag_k;
            return result;
        }
        result.flag = flag;

        switch_t* const* toggle = std::get_if<switch_t*>(&flag->parsed_variant);
        if (toggle && equals != std::string_view::npos) {
            result.value = argument.substr(equals + 1);
            result.status = argument_status_t::unexpected_value_k;
            return result;
        }
        if (toggle) {
            **toggle = switch_t::present_k;
            continue;
        }
        if (equals == std::string_view::npos && index + 1 == argc) {
            result.status = argument_status_t::missing_value_k;
            return result;
        }

        result.value = equals != std::string_view::npos ? argument.substr(equals + 1) : argv[++index];
        result.status = assign_(*flag, result.value);
        if (result.status != argument_status_t::parsed_k) return result;
    }
    result = {.positionals_count = result.positionals_count};
    return result;
}

/** The grammar a value of @p flag parses in, as the error line names it. */
[[nodiscard]] inline std::string_view grammar(flag_t const& flag) noexcept {
    if (count_t* const* count = std::get_if<count_t*>(&flag.parsed_variant))
        return (*count)->minimum ? "a positive count" : "an unsigned integer";
    if (counts_t* const* counts = std::get_if<counts_t*>(&flag.parsed_variant))
        return (*counts)->minimum ? "positive counts like 64,128" : "unsigned integers like 0,1";
    if (std::holds_alternative<std::chrono::milliseconds*>(flag.parsed_variant)) return "a duration like 200ms or 10s";
    if (std::holds_alternative<std::uint32_t*>(flag.parsed_variant)) return "an unsigned integer or random";
    if (std::holds_alternative<double*>(flag.parsed_variant)) return "a non-negative real";
    if (std::holds_alternative<regex_filter_t*>(flag.parsed_variant)) return "an ECMAScript regex";
    if (std::holds_alternative<texts_t*>(flag.parsed_variant)) return "names separated by commas";
    if (std::holds_alternative<choice_t*>(flag.parsed_variant)) return "one of the names --help lists";
    if (std::holds_alternative<switch_t*>(flag.parsed_variant)) return "no value";
    if (std::holds_alternative<bytes_t*>(flag.parsed_variant)) return "bytes like 4096 or 24G";
    return "a value";
}

/** The variable that sets @p flag's default, the name the Python suites read too: <tt>--seed</tt>
 *  reads @c USTORE_SEED. */
[[nodiscard]] inline sz::string_t ustore_variable(flag_t const& flag) noexcept {
    sz::string_t variable = formatted("USTORE_{}", flag.name.substr(2));
    for (std::size_t letter = std::string_view {"USTORE_"}.size(); letter != variable.size(); ++letter)
        variable[letter] = variable[letter] == '-'
                               ? '_'
                               : static_cast<char>(std::toupper(static_cast<unsigned char>(variable[letter])));
    return variable;
}

/** Writes @p value, a variable's text, into @p flag: a switch takes 1, 0, true or false, the rest
 *  parse as on the command line. */
[[nodiscard]] inline argument_status_t assign_variable_(flag_t const& flag, std::string_view value) noexcept {
    switch_t* const* toggle = std::get_if<switch_t*>(&flag.parsed_variant);
    if (!toggle) return assign_(flag, value);
    bool const present = value == "1" || value == "true";
    if (!present && value != "0" && value != "false") return argument_status_t::unexpected_value_k;
    **toggle = present ? switch_t::present_k : switch_t::absent_k;
    return argument_status_t::parsed_k;
}

/** Prints @p synopsis and one aligned line per flag, generated from the same table that parses. */
inline void print_usage(std::string_view synopsis, std::span<flag_t const> flags) {
    constexpr std::size_t column_k = 34;
    fmt::println("Usage: {}\n", synopsis);
    for (flag_t const& flag : flags) {
        std::size_t const width = flag.name.size() + (flag.placeholder.empty() ? 0 : flag.placeholder.size() + 3);
        std::size_t const padding = width < column_k ? column_k - width : 1;
        if (flag.placeholder.empty())
            fmt::println("  {}{:{}}{} [env: {}]", flag.name, "", padding, flag.help, ustore_variable(flag));
        else
            fmt::println("  {} <{}>{:{}}{} [env: {}]", flag.name, flag.placeholder, "", padding, flag.help,
                         ustore_variable(flag));
    }
    fmt::println("  --help{:{}}print this message", "", column_k - 6);
}

/** Parses @p argv against @p flags, each defaulting to its @ref ustore_variable when set, so one
 *  environment drives these binaries and the Python suites alike. On <tt>--help</tt> prints the usage
 *  and exits with status 0; on the first variable or argument that fails prints one line, like
 *  <tt>--threads="0" does not parse, expected a positive count</tt>, and exits with status 1. */
inline void parse_or_exit(int argc, char const* const* argv, std::string_view synopsis, std::span<flag_t const> flags) {
    for (flag_t const& flag : flags) {
        sz::string_t const variable = ustore_variable(flag);
        char const* const value = std::getenv(variable.c_str());
        if (!value || !*value || assign_variable_(flag, value) == argument_status_t::parsed_k) continue;
        bool const toggle = std::holds_alternative<switch_t*>(flag.parsed_variant);
        fmt::println(stderr, "{}=\"{}\" does not parse, expected {}", variable, value,
                     toggle ? "1, 0, true or false" : grammar(flag));
        std::exit(1);
    }
    arguments_t const parsed = parse_arguments(argc, argv, flags, {});
    if (parsed.status == argument_status_t::parsed_k) return;
    if (parsed.status == argument_status_t::help_k) {
        print_usage(synopsis, flags);
        std::exit(0);
    }
    if (!parsed.flag) fmt::println(stderr, "{} is not a flag, see --help", parsed.argument);
    else
        fmt::println(stderr, "{}=\"{}\" does not parse, expected {}", parsed.flag->name, parsed.value,
                     grammar(*parsed.flag));
    std::exit(1);
}

#pragma region Echo

/** A flag value spelled into a buffer of its own, so a whole line prints in one call. */
struct spelling_t {
    std::array<char, 256> text {};
    std::size_t size = 0;

    template <typename... arguments_types_>
    spelling_t(fmt::format_string<arguments_types_...> format, arguments_types_&&... arguments) noexcept {
        size = fmt::format_to_n(text.data(), text.size(), format, std::forward<arguments_types_>(arguments)...).size;
        if (size > text.size()) size = text.size();
    }

    [[nodiscard]] std::string_view view() const noexcept { return {text.data(), size}; }
};

} // namespace unum::ustore::tools

/** The short spelling the foreign specialization below reaches the project's types through. */
namespace us = unum::ustore;

/** Spells a @ref us::tools::bytes_t as a user types it, in the largest binary suffix dividing
 *  it, like "256M". */
template <>
struct fmt::formatter<us::tools::bytes_t> : fmt::formatter<std::string_view> {
    auto format(us::tools::bytes_t size, fmt::format_context& context) const {
        constexpr std::array<char const*, 5> suffixes {"", "K", "M", "G", "T"};
        std::size_t bytes = size.value, power = 0;
        while (power + 1 != suffixes.size() && bytes && bytes % 1024 == 0) bytes /= 1024, ++power;
        return fmt::format_to(context.out(), "{}{}", bytes, suffixes[power]);
    }
};

namespace unum::ustore::tools {

/** Spells the value of @p flag as a user types it, or "none" for empty text. */
inline spelling_t spell_value_(flag_t const& flag) noexcept {
    if (std::string_view* const* text = std::get_if<std::string_view*>(&flag.parsed_variant))
        return {"{}", (*text)->empty() ? std::string_view {"none"} : **text};
    if (count_t* const* count = std::get_if<count_t*>(&flag.parsed_variant)) return {"{}", (*count)->value};
    if (counts_t* const* counts = std::get_if<counts_t*>(&flag.parsed_variant))
        return {"{}", fmt::join((*counts)->view(), ",")};
    if (switch_t* const* toggle = std::get_if<switch_t*>(&flag.parsed_variant))
        return {"{}", **toggle == switch_t::present_k ? "true" : "false"};
    if (std::chrono::milliseconds* const* duration = std::get_if<std::chrono::milliseconds*>(&flag.parsed_variant))
        return {"{}", **duration};
    if (std::uint32_t* const* seed = std::get_if<std::uint32_t*>(&flag.parsed_variant)) return {"{}", **seed};
    if (double* const* real = std::get_if<double*>(&flag.parsed_variant)) return {"{}", **real};
    if (regex_filter_t* const* filter = std::get_if<regex_filter_t*>(&flag.parsed_variant))
        return {"{}", (*filter)->text.empty() ? std::string_view {"none"} : (*filter)->text};
    if (texts_t* const* texts = std::get_if<texts_t*>(&flag.parsed_variant)) {
        if ((*texts)->size) return {"{}", fmt::join((*texts)->view(), ",")};
        return {"none"};
    }
    if (choice_t* const* choice = std::get_if<choice_t*>(&flag.parsed_variant))
        return {"{}", (*choice)->names[(*choice)->selected]};
    return {"{}", **std::get_if<bytes_t*>(&flag.parsed_variant)};
}

/** Prints every flag as "- Label: value" in the grammar it parses from, labelled like
 *  "--time-limit" as "Time limit", skipping a flag that a later one of the same name shadows, so a
 *  suite's log names every setting it ran with. */
inline void print_flags(std::span<flag_t const> flags) {
    for (std::size_t index = 0; index != flags.size(); ++index) {
        flag_t const& flag = flags[index];
        bool shadowed = false;
        for (flag_t const& later : flags.subspan(index + 1)) shadowed = shadowed || later.name == flag.name;
        if (shadowed) continue;
        spelling_t label {"{}", flag.name.substr(2)};
        for (char& character : label.text)
            if (character == '-') character = ' ';
        label.text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label.text[0])));
        fmt::println("- {}: {}", label.view(), spell_value_(flag).view());
    }
}

#pragma endregion Echo

#pragma region Engines

/** The database name @c nvme gives the manifest it opens in @c engine_options_t::directory. */
inline constexpr std::string_view nvme_database_name_k = "ustore";

/** Bytes the longest manifest path of @c nvme_manifest_in takes, its terminator included. */
inline constexpr std::size_t nvme_manifest_capacity_k = 4096;

/** The manifest @c nvme opens at @p directory, written terminated into @p into:
 *  @c <directory>/ustore.ustore.json for a plain directory, and @p directory itself for a raw
 *  address or a path to a manifest. */
inline std::string_view nvme_manifest_in(std::string_view directory, std::span<char> into) noexcept {
    auto const [address, address_status] = util::parse_data_address(directory);
    bool const named = address_status.failed() || address.kind != util::data_address_kind_t::directory_k ||
                       directory.ends_with(util::manifest_suffix_k);
    auto const written = named ? fmt::format_to_n(into.data(), into.size() - 1, "{}", directory)
                               : fmt::format_to_n(into.data(), into.size() - 1, "{}/{}{}", directory,
                                                  nvme_database_name_k, util::manifest_suffix_k);
    *written.out = '\0';
    return std::string_view {into.data(), static_cast<std::size_t>(written.out - into.data())};
}

/**
 *  @brief The engine a binary opens and the options its flags give the engine's init call.
 *
 *  The engine, the isolation and the transport stay text until @c open_engine reads them through
 *  the C interface's own spellings, so a suite built against the headers alone parses the same
 *  flags without linking the library.
 */
struct engine_options_t {

    /** The engine's name: @c ram, @c rocksdb, @c leveldb or @c nvme. */
    std::string_view name = "ram";

    /** How the transactions of @c ram see each other, as @c ustore_isolation_name spells it. */
    std::string_view isolation = "strict-serializable";

    /** How @c nvme moves bytes between files and memory, as @c ustore_transport_name spells it. */
    std::string_view transport = "automatic";

    /** The null-terminated database directory, or null for an in-memory database. */
    char const* directory = nullptr;

    /** Directories the runs of @c nvme or the table files of @c rocksdb fill, one per drive;
     *  none keeps them in @c directory. */
    texts_t directories;

    /** The manifest @c nvme opens in @c directory, kept here so a deferred open still reads it. */
    std::array<char, nvme_manifest_capacity_k> nvme_manifest {};

    /** Threads of the executor @c spawn_executor makes, zero for the cores of the affinity mask. */
    count_t threads {.value = 0, .minimum = 0};

    /** Points the engine at @p path, a directory or null, and spells the manifest
     *  @c nvme opens there. */
    void locate(char const* path) noexcept {
        directory = path;
        if (path) [[maybe_unused]]
            std::string_view const spelled = nvme_manifest_in(path, nvme_manifest);
        else nvme_manifest[0] = '\0';
    }
};

/** The engine, isolation and transport an @c engine_options_t names, read through C's spellings. */
struct engine_names_t {
    ustore_engine_t engine {};
    ustore_isolation_t isolation {};
    ustore_transport_t transport {};
};

/** The flag of an @c engine_options_t whose text names nothing, with the grammar it expects. */
struct unnamed_flag_t {
    std::string_view flag;
    std::string_view value;
    std::string_view grammar;
};

/** Reads the names of @p options into @p names through the C interface's own spellings,
 *  answering the first that names nothing, or an empty flag when every one parses. */
[[nodiscard]] inline unnamed_flag_t read_engine_names(engine_options_t const& options, engine_names_t& names) noexcept {
    if (assign_named_(options.name, names.engine, &ustore_engine_from_name) != argument_status_t::parsed_k)
        return {"--engine", options.name, "ram, rocksdb, leveldb or nvme"};
    if (assign_named_(options.isolation, names.isolation, &ustore_isolation_from_name) != argument_status_t::parsed_k)
        return {"--isolation", options.isolation, "strict-serializable or snapshot"};
    if (assign_named_(options.transport, names.transport, &ustore_transport_from_name) != argument_status_t::parsed_k)
        return {"--engine-transport", options.transport, "automatic, blocking, linux-uring, linux-aio or linux-spdk"};
    return {};
}

/** Cores of the process's affinity mask, at least one, each a thread of a binary's executor. */
inline std::size_t affinity_cores() noexcept {
#if USTORE_OS_LINUX_
    cpu_set_t mask;
    if (sched_getaffinity(0, sizeof(mask), &mask) == 0 && CPU_COUNT(&mask) > 0)
        return static_cast<std::size_t>(CPU_COUNT(&mask));
#endif
    unsigned const cores = std::thread::hardware_concurrency();
    return cores ? cores : 1;
}

/** Bytes of memory the process's cgroup and its ancestors allow, the smallest @c memory.max among
 *  them, or zero where none sets one or none can be read, as off Linux. */
inline std::size_t cgroup_memory_limit() noexcept {
    std::size_t limit = 0;
#if USTORE_OS_LINUX_
    st::vector<char> membership;
    if (util::read_file("/proc/self/cgroup", membership).failed()) return 0;
    std::string_view const text {membership.begin(), membership.end()};
    std::size_t const unified = text.find("0::");
    if (unified == std::string_view::npos) return 0;
    std::string_view group = text.substr(unified + 3);
    group = group.substr(0, group.find('\n'));
    for (;;) {
        char path[util::path_max_bytes_k];
        auto const written = fmt::format_to_n(path, sizeof(path) - 1, "/sys/fs/cgroup{}/memory.max", group);
        *written.out = '\0';
        st::vector<char> value;
        std::size_t bytes = 0;
        if (util::read_file(path, value).succeeded() &&
            std::from_chars(value.data(), value.data() + value.size(), bytes).ec == std::errc {} &&
            (!limit || bytes < limit))
            limit = bytes;
        if (group.empty() || group == "/") break;
        group = group.substr(0, group.rfind('/'));
    }
#endif
    return limit;
}

/** Spawns into @p executor the one queue set a binary runs every call on: an executor of
 *  @c ustore_executor_for_nvme moving bytes as @p options name for @c nvme, one of
 *  @c ustore_executor_for_ram for every other engine, with @c threads threads, whose scratch
 *  together takes a quarter of the cgroup's memory, or the library's default without one. */
inline void spawn_executor(engine_options_t const& options, ustore_executor_t* executor,
                           ustore_outcome_t* outcome) noexcept {
    engine_names_t names;
    if (unnamed_flag_t const unnamed = read_engine_names(options, names); !unnamed.flag.empty()) {
        *outcome = {ustore_status_invalid_argument_k, "Unknown engine, isolation or transport name"};
        return;
    }
    std::size_t const threads = options.threads.value ? options.threads.value : affinity_cores();
    // Every pool thread and the call itself keep a scratch
    std::size_t const scratch_limit = cgroup_memory_limit() / 4 / (threads + 1);
    if (names.engine == ustore_engine_nvme_k) {
        ustore_executor_for_nvme_t spawn {.executor = executor,
                                          .outcome = outcome,
                                          .nvme = {.transport = names.transport},
                                          .threads = threads,
                                          .scratch_limit = scratch_limit};
        return ustore_executor_for_nvme(&spawn);
    }
    ustore_executor_for_ram_t spawn {.executor = executor,
                                     .outcome = outcome,
                                     .threads = threads,
                                     .scratch_limit = scratch_limit};
    ustore_executor_for_ram(&spawn);
}

/** Opens the engine @p options name into @p database, running the open on @p executor with
 *  @p completion as every call does; the one place binaries tell the engines apart. The in-memory
 *  engine starts empty without a directory, and persists in one otherwise. NVMe moves bytes as the
 *  executor @c spawn_executor made for it names, so a transport named without one is refused. Every
 *  engine may hold half the cgroup's memory, or the library's default without a cgroup limit. */
inline void open_engine(engine_options_t const& options, ustore_executor_t executor, ustore_database_t* database,
                        ustore_outcome_t* outcome,
                        ustore_completion_t completion = ustore_completion_blocking_k) noexcept {
    engine_names_t names;
    if (unnamed_flag_t const unnamed = read_engine_names(options, names); !unnamed.flag.empty()) {
        *outcome = {ustore_status_invalid_argument_k, "Unknown engine, isolation or transport name"};
        return;
    }
    ustore_bytes_view_t const directory {reinterpret_cast<ustore_byte_t const*>(options.directory),
                                         options.directory ? std::strlen(options.directory) : 0};
    std::array<ustore_data_directory_t, texts_t::capacity_k> data_directories {};
    for (std::size_t index = 0; index != options.directories.size; ++index) {
        std::string_view const path = options.directories.values[index];
        data_directories[index].path = {reinterpret_cast<ustore_byte_t const*>(path.data()), path.size()};
    }
    ustore_data_directory_t const* const listed = options.directories.size ? data_directories.data() : nullptr;
    if (listed && names.engine != ustore_engine_nvme_k && names.engine != ustore_engine_rocksdb_k) {
        *outcome = {ustore_status_invalid_argument_k, "Only nvme and rocksdb fill data directories"};
        return;
    }
    std::size_t const memory_limit = cgroup_memory_limit() / 2;
    switch (names.engine) {
    case ustore_engine_ram_k: {
        if (!directory.length) {
            ustore_database_init_ram_t init {.database = database,
                                             .executor = executor,
                                             .outcome = outcome,
                                             .completion = completion,
                                             .isolation = names.isolation,
                                             .memory_limit = memory_limit};
            return ustore_database_init_ram(&init);
        }
        ustore_database_open_ram_t open {.database = database,
                                         .executor = executor,
                                         .outcome = outcome,
                                         .completion = completion,
                                         .directory = directory,
                                         .isolation = names.isolation,
                                         .memory_limit = memory_limit};
        return ustore_database_open_ram(&open);
    }
    case ustore_engine_rocksdb_k: {
        ustore_database_open_rocksdb_t open {.database = database,
                                             .executor = executor,
                                             .outcome = outcome,
                                             .completion = completion,
                                             .directory = directory,
                                             .data_directories = listed,
                                             .data_directories_count = options.directories.size,
                                             .memory_limit = memory_limit};
        return ustore_database_open_rocksdb(&open);
    }
    case ustore_engine_leveldb_k: {
        ustore_database_open_leveldb_t open {.database = database,
                                             .executor = executor,
                                             .outcome = outcome,
                                             .completion = completion,
                                             .directory = directory,
                                             .memory_limit = memory_limit};
        return ustore_database_open_leveldb(&open);
    }
    case ustore_engine_nvme_k: {
        std::string_view const located = directory.length ? std::string_view {options.nvme_manifest.data()} : "";
        ustore_database_open_nvme_t open {.database = database,
                                          .executor = executor,
                                          .outcome = outcome,
                                          .completion = completion,
                                          .directory = bytes_view(located),
                                          .data_directories = listed,
                                          .data_directories_count = options.directories.size,
                                          .memory_limit = memory_limit};
        if (!executor && names.transport != ustore_transport_automatic_k) {
            *outcome = {ustore_status_invalid_argument_k, "A transport needs an NVMe executor"};
            return;
        }
        return ustore_database_open_nvme(&open);
    }
    default: *outcome = {ustore_status_invalid_argument_k, "No engine has that name"};
    }
}

#pragma endregion Engines

} // namespace unum::ustore::tools
