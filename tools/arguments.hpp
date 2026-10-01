/**
 *  @file tools/arguments.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief Command-line flags for every UStore binary: one table parses the arguments, echoes them and prints the usage.
 *
 *  Flags are long kebab-case words whose value follows as the next argument or after @c =. Sizes carry binary
 *  suffixes of any case, durations carry @c ms or @c s, counts are positive integers, and lists are comma-separated.
 *  A text value views the tail of its @c argv entry, so it is null-terminated; list items are not.
 *  A value that does not parse prints @c --flag="value" @c does @c not @c parse, @c expected @c <grammar>.
 *
 *  @code
 *  bytes_t memory_limit {64 << 20};
 *  std::string_view directory;
 *  std::array<std::string_view, 2> const formats {"ndjson", "csv"};
 *  choice_t format {formats};
 *  std::array<flag_t, 3> const flags {{
 *      {"--directory", "path", "database directory", &directory},
 *      {"--memory-limit", "size", "arena budget, like 64MB", &memory_limit},
 *      {"--format", "name", "export format", &format},
 *  }};
 *  arguments_t const parsed = parse_arguments(argc, argv, flags, {});
 *  @endcode
 */
#pragma once

#include <cctype>  // `std::tolower`, `std::toupper`
#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint32_t`, `std::uint64_t`
#include <cstdio>  // `stderr`

#include <algorithm>    // `std::any_of`, `std::equal`
#include <array>        // `std::array`
#include <charconv>     // `std::from_chars`
#include <chrono>       // `std::chrono::nanoseconds`
#include <limits>       // `std::numeric_limits`
#include <optional>     // `std::optional`
#include <random>       // `std::random_device`
#include <regex>        // `std::regex`, `std::regex_search`
#include <span>         // `std::span`
#include <string>       // `std::string`, `std::to_string`
#include <string_view>  // `std::string_view`
#include <system_error> // `std::errc`
#include <variant>      // `std::variant`

#include <fmt/format.h>
#include <fmt/ranges.h> // `fmt::join`

#include "ustore/db.h" // the init call of every engine and its arguments

namespace unum::ustore::tools {

/** Outcome of parsing one value or a whole argument list. */
enum class argument_status_t {

    /** Every argument matched its flag. */
    parsed_k,

    /** `--help` was given, so the caller prints the usage and exits. */
    help_k,

    /** An argument starting with `--` names no flag. */
    unknown_flag_k,

    /** A flag taking a value ended the argument list. */
    missing_value_k,

    /** A switch was given a value through `=`. */
    unexpected_value_k,

    /** The digits do not form a non-negative integer. */
    invalid_number_k,

    /** The suffix names no unit of the flag's kind. */
    invalid_unit_k,

    /** The value names none of the flag's choices. */
    invalid_choice_k,

    /** The value overflows its destination or falls outside its bounds. */
    out_of_range_k,

    /** A list or the positional arguments exceed their capacity. */
    too_many_items_k,

    /** A list holds fewer items than it requires. */
    too_few_items_k,
};

/** A positive size in bytes, written @c 64MB or @c 1gb; a bare number is bytes. */
struct bytes_t {
    std::size_t value = 0;
};

/** A plain integer, accepted only within @b [minimum,maximum]. */
struct count_t {
    std::size_t value = 0;
    std::size_t minimum = 1;
    std::size_t maximum = std::numeric_limits<std::size_t>::max();
};

/** Comma-separated counts like @c 16,256,4096, each within @b [minimum,maximum].
 *  Between @c minimum_size and @c maximum_size of them are accepted. */
struct counts_t {
    static constexpr std::size_t capacity_k = 16;
    std::array<std::size_t, capacity_k> values {};
    std::size_t size = 0;
    std::size_t minimum = 1;
    std::size_t maximum = std::numeric_limits<std::size_t>::max();
    std::size_t minimum_size = 1;
    std::size_t maximum_size = capacity_k;

    [[nodiscard]] std::span<std::size_t const> view() const noexcept { return {values.data(), size}; }
};

/** Comma-separated text items like @c /mnt/a,/mnt/b, viewing into @c argv without null terminators. */
struct texts_t {
    static constexpr std::size_t capacity_k = 16;
    std::array<std::string_view, capacity_k> values {};
    std::size_t size = 0;

    [[nodiscard]] std::span<std::string_view const> view() const noexcept { return {values.data(), size}; }
};

/** One name out of @c names, like @c ndjson|csv; @c selected indexes the chosen one. */
struct choice_t {
    std::span<std::string_view const> names;
    std::size_t selected = 0;
};

/** A 32-bit seed, or @c random to draw one from the machine. */
struct seed_t {
    std::uint32_t value = 42;
};

/** A regex over names, found as a plain substring when it does not compile; empty selects everything. */
struct filter_t {
    std::string_view text;
    std::optional<std::regex> pattern;

    [[nodiscard]] bool selects(std::string_view name) const noexcept {
        if (text.empty()) return true;
        if (pattern) return std::regex_search(name.begin(), name.end(), *pattern);
        return name.find(text) != std::string_view::npos;
    }
};

/** Whether a flag without a value appeared. */
enum class switch_t : bool {
    absent_k,
    present_k,
};

/** One flag: its name with both dashes, the usage placeholder, one help line, the value it writes, and the label
 *  its echo takes, derived from the name when empty. */
struct flag_t {
    std::string_view name;
    std::string_view placeholder;
    std::string_view help;
    std::variant<std::string_view*, count_t*, counts_t*, bytes_t*, std::chrono::nanoseconds*, texts_t*, choice_t*,
                 switch_t*, seed_t*, filter_t*>
        destination;
    std::string_view label;
};

/** Result of @c parse_arguments: the status, the argument it concerns, the flag that argument named and the value
 *  it gave, and how many positionals were collected. */
struct arguments_t {
    argument_status_t status = argument_status_t::parsed_k;
    std::string_view argument;
    flag_t const* flag = nullptr;
    std::string_view value;
    std::size_t positionals_count = 0;
};

#pragma region Values

/** A suffix and the multiplier it applies to the digits before it. */
struct unit_t {
    std::string_view suffix;
    std::uint64_t multiplier = 1;
};

inline constexpr std::array<unit_t, 2> duration_units_k {{
    {"ms", 1'000'000},
    {"s", 1'000'000'000},
}};

[[nodiscard]] inline argument_status_t parse_integer_(std::string_view digits, std::uint64_t& value) noexcept {
    if (digits.empty()) return argument_status_t::invalid_number_k;
    auto const [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    if (error == std::errc::result_out_of_range) return argument_status_t::out_of_range_k;
    if (error != std::errc {} || end != digits.data() + digits.size()) return argument_status_t::invalid_number_k;
    return argument_status_t::parsed_k;
}

[[nodiscard]] inline argument_status_t parse_with_units_(std::string_view text, std::span<unit_t const> units,
                                                         std::uint64_t& value) noexcept {
    std::size_t digits_length = 0;
    while (digits_length != text.size() && text[digits_length] >= '0' && text[digits_length] <= '9') ++digits_length;
    std::uint64_t number = 0;
    argument_status_t const status = parse_integer_(text.substr(0, digits_length), number);
    if (status != argument_status_t::parsed_k) return status;

    std::string_view const suffix = text.substr(digits_length);
    for (unit_t const& unit : units) {
        if (unit.suffix != suffix) continue;
        if (number != 0 && unit.multiplier > std::numeric_limits<std::uint64_t>::max() / number)
            return argument_status_t::out_of_range_k;
        value = number * unit.multiplier;
        return argument_status_t::parsed_k;
    }
    return argument_status_t::invalid_unit_k;
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

/** A positive size in whole bytes or binary units of any case, like "4096", "64KB" or "1GB". */
inline std::optional<std::size_t> parse_size(std::string_view text) noexcept {
    std::size_t bytes = 0;
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), bytes);
    if (error != std::errc {} || !bytes) return std::nullopt;
    std::string_view const unit = text.substr(end - text.data());
    auto const same_letter = [](char given, char lower) noexcept {
        return std::tolower(static_cast<unsigned char>(given)) == lower;
    };
    for (std::string_view const known : std::array<std::string_view, 5> {"", "kb", "mb", "gb", "tb"}) {
        if (std::equal(unit.begin(), unit.end(), known.begin(), known.end(), same_letter)) return bytes;
        if (bytes > std::numeric_limits<std::size_t>::max() / 1024) return std::nullopt;
        bytes *= 1024;
    }
    return std::nullopt;
}

/** Parses a size such as @c 64MB into bytes; zero is rejected. */
[[nodiscard]] inline argument_status_t parse_bytes(std::string_view text, bytes_t& bytes) noexcept {
    std::optional<std::size_t> const size = parse_size(text);
    if (!size) return argument_status_t::invalid_unit_k;
    bytes.value = *size;
    return argument_status_t::parsed_k;
}

/** Parses a duration such as @c 500ms or @c 10s; a bare number, a fraction or zero is rejected. */
[[nodiscard]] inline argument_status_t parse_duration(std::string_view text,
                                                      std::chrono::nanoseconds& duration) noexcept {
    std::uint64_t value = 0;
    argument_status_t const status = parse_with_units_(text, duration_units_k, value);
    if (status != argument_status_t::parsed_k) return status;
    if (value == 0 || value > static_cast<std::uint64_t>(std::numeric_limits<std::chrono::nanoseconds::rep>::max()))
        return argument_status_t::out_of_range_k;
    duration = std::chrono::nanoseconds {static_cast<std::chrono::nanoseconds::rep>(value)};
    return argument_status_t::parsed_k;
}

/** Parses a plain integer within the bounds @p count already carries. */
[[nodiscard]] inline argument_status_t parse_count(std::string_view text, count_t& count) noexcept {
    return parse_bounded_(text, count.minimum, count.maximum, count.value);
}

/** Parses comma-separated counts within the bounds @p counts carries, replacing the items it held. */
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

/** Parses a seed: an unsigned 32-bit integer, or @c random for a fresh draw from @c std::random_device. */
[[nodiscard]] inline argument_status_t parse_seed(std::string_view text, seed_t& seed) noexcept {
    if (text == "random") {
        seed.value = static_cast<std::uint32_t>(std::random_device {}());
        return argument_status_t::parsed_k;
    }
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), seed.value);
    if (error == std::errc::result_out_of_range) return argument_status_t::out_of_range_k;
    if (error != std::errc {} || end != text.data() + text.size()) return argument_status_t::invalid_number_k;
    return argument_status_t::parsed_k;
}

/** Keeps @p text as the filter, compiled once as an ECMAScript regex when it compiles; any text parses. */
[[nodiscard]] inline argument_status_t parse_filter(std::string_view text, filter_t& filter) noexcept {
    filter.text = text;
    filter.pattern.reset();
    try {
        filter.pattern.emplace(text.begin(), text.end());
    }
    catch (std::regex_error const&) {
        // Left empty, so `selects` finds the filter as a plain substring instead.
    }
    return argument_status_t::parsed_k;
}

/** Selects the name in @p choice equal to @p text. */
[[nodiscard]] inline argument_status_t parse_choice(std::string_view text, choice_t& choice) noexcept {
    for (std::size_t index = 0; index != choice.names.size(); ++index) {
        if (choice.names[index] != text) continue;
        choice.selected = index;
        return argument_status_t::parsed_k;
    }
    return argument_status_t::invalid_choice_k;
}

#pragma endregion Values

#pragma region Arguments

[[nodiscard]] inline argument_status_t assign_(flag_t const& flag, std::string_view value) noexcept {
    if (auto const* text = std::get_if<std::string_view*>(&flag.destination)) {
        **text = value;
        return argument_status_t::parsed_k;
    }
    if (auto const* count = std::get_if<count_t*>(&flag.destination)) return parse_count(value, **count);
    if (auto const* counts = std::get_if<counts_t*>(&flag.destination)) return parse_counts(value, **counts);
    if (auto const* bytes = std::get_if<bytes_t*>(&flag.destination)) return parse_bytes(value, **bytes);
    if (auto const* duration = std::get_if<std::chrono::nanoseconds*>(&flag.destination))
        return parse_duration(value, **duration);
    if (auto const* texts = std::get_if<texts_t*>(&flag.destination)) return parse_texts(value, **texts);
    if (auto const* choice = std::get_if<choice_t*>(&flag.destination)) return parse_choice(value, **choice);
    if (auto const* seed = std::get_if<seed_t*>(&flag.destination)) return parse_seed(value, **seed);
    if (auto const* filter = std::get_if<filter_t*>(&flag.destination)) return parse_filter(value, **filter);
    return argument_status_t::unexpected_value_k;
}

/**
 *  @brief Parses @p argv against @p flags, collecting arguments without leading dashes into @p positionals.
 *
 *  Parsing stops at the first failure, leaving earlier destinations written.
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
        result.argument = argument;

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

        auto const* toggle = std::get_if<switch_t*>(&flag->destination);
        if (toggle && equals != std::string_view::npos) {
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

        std::string_view const value = equals != std::string_view::npos ? argument.substr(equals + 1) : argv[++index];
        result.status = assign_(*flag, value);
        if (result.status != argument_status_t::parsed_k) {
            result.flag = flag;
            result.value = value;
            return result;
        }
    }
    result.argument = {};
    return result;
}

/** A one-line explanation of @p status for error messages. */
[[nodiscard]] constexpr std::string_view describe(argument_status_t status) noexcept {
    switch (status) {
    case argument_status_t::parsed_k: return "parsed";
    case argument_status_t::help_k: return "help requested";
    case argument_status_t::unknown_flag_k: return "unknown flag";
    case argument_status_t::missing_value_k: return "missing value";
    case argument_status_t::unexpected_value_k: return "this flag takes no value";
    case argument_status_t::invalid_number_k: return "not a non-negative integer";
    case argument_status_t::invalid_unit_k: return "unknown unit";
    case argument_status_t::invalid_choice_k: return "not one of the accepted names";
    case argument_status_t::out_of_range_k: return "value out of range";
    case argument_status_t::too_many_items_k: return "too many values";
    case argument_status_t::too_few_items_k: return "too few values";
    }
    return "unknown status";
}

/** Prints @p synopsis and one aligned line per flag, generated from the same table that parses. */
inline void print_usage(std::string_view synopsis, std::span<flag_t const> flags) {
    constexpr std::size_t column_k = 34;
    fmt::println("Usage: {}\n", synopsis);
    for (flag_t const& flag : flags) {
        std::size_t const width = flag.name.size() + (flag.placeholder.empty() ? 0 : flag.placeholder.size() + 3);
        std::size_t const padding = width < column_k ? column_k - width : 1;
        std::span<std::string_view const> names;
        if (auto const* choice = std::get_if<choice_t*>(&flag.destination)) names = (*choice)->names;
        std::string_view const separator = names.empty() ? "" : ": ";
        if (flag.placeholder.empty())
            fmt::println("  {}{:{}}{}{}{}", flag.name, "", padding, flag.help, separator, fmt::join(names, "|"));
        else
            fmt::println("  {} <{}>{:{}}{}{}{}", flag.name, flag.placeholder, "", padding, flag.help, separator,
                         fmt::join(names, "|"));
    }
    fmt::println("  --help{:{}}print this message", "", column_k - 6);
}

/** Spells @p duration as a user types it: whole seconds as "10s", anything else as "1500ms". */
inline std::string spell_duration(std::chrono::milliseconds duration) {
    auto const count = duration.count();
    return count % 1000 ? std::to_string(count) + "ms" : std::to_string(count / 1000) + "s";
}

/** Spells @p bytes as a user types it, in the largest binary unit dividing it, like "256MB". */
inline std::string spell_size(std::size_t bytes) {
    constexpr std::array<char const*, 5> units {"", "KB", "MB", "GB", "TB"};
    std::size_t power = 0;
    while (power + 1 != units.size() && bytes && bytes % 1024 == 0) bytes /= 1024, ++power;
    return std::to_string(bytes) + units[power];
}

/** The grammar the value of @p flag takes, which a failure line ends with. */
[[nodiscard]] inline std::string expected_(flag_t const& flag) {
    if (auto const* count = std::get_if<count_t*>(&flag.destination)) {
        std::size_t const minimum = (*count)->minimum, maximum = (*count)->maximum;
        if (maximum != std::numeric_limits<std::size_t>::max())
            return fmt::format("a count from {} to {}", minimum, maximum);
        return minimum == 1 ? "a positive count" : fmt::format("a count of at least {}", minimum);
    }
    if (auto const* counts = std::get_if<counts_t*>(&flag.destination))
        return (*counts)->minimum_size == (*counts)->maximum_size
                   ? fmt::format("{} positive counts like 32,64,128", (*counts)->minimum_size)
                   : "positive counts like 64,128";
    if (std::holds_alternative<bytes_t*>(flag.destination)) return "a size like 4096, 64KB or 1GB";
    if (std::holds_alternative<std::chrono::nanoseconds*>(flag.destination)) return "a duration like 200ms or 10s";
    if (std::holds_alternative<seed_t*>(flag.destination)) return "an unsigned integer or random";
    if (auto const* choice = std::get_if<choice_t*>(&flag.destination))
        return fmt::format("one of {}", fmt::join((*choice)->names, ", "));
    return "names separated by commas";
}

/** Prints why @p parsed failed as one line, like @c --threads="0" @c does @c not @c parse, @c expected @c a @c positive
 *  @c count, when a value failed, or as the argument and the reason otherwise. */
inline void print_error(arguments_t const& parsed) {
    if (!parsed.flag) return fmt::println(stderr, "{}: {}", parsed.argument, describe(parsed.status));
    fmt::println(stderr, "{}=\"{}\" does not parse, expected {}", parsed.flag->name, parsed.value,
                 expected_(*parsed.flag));
}

/** The value of @p flag, spelled as a user types it, or "none" for empty text. */
[[nodiscard]] inline std::string spell_value_(flag_t const& flag) {
    if (auto const* text = std::get_if<std::string_view*>(&flag.destination))
        return (*text)->empty() ? "none" : std::string(**text);
    if (auto const* count = std::get_if<count_t*>(&flag.destination)) return std::to_string((*count)->value);
    if (auto const* counts = std::get_if<counts_t*>(&flag.destination))
        return fmt::format("{}", fmt::join((*counts)->view(), ","));
    if (auto const* bytes = std::get_if<bytes_t*>(&flag.destination)) return spell_size((*bytes)->value);
    if (auto const* duration = std::get_if<std::chrono::nanoseconds*>(&flag.destination))
        return spell_duration(std::chrono::duration_cast<std::chrono::milliseconds>(**duration));
    if (auto const* texts = std::get_if<texts_t*>(&flag.destination))
        return (*texts)->size ? fmt::format("{}", fmt::join((*texts)->view(), ",")) : "none";
    if (auto const* choice = std::get_if<choice_t*>(&flag.destination))
        return std::string((*choice)->names[(*choice)->selected]);
    if (auto const* toggle = std::get_if<switch_t*>(&flag.destination))
        return **toggle == switch_t::present_k ? "true" : "false";
    if (auto const* seed = std::get_if<seed_t*>(&flag.destination)) return std::to_string((*seed)->value);
    filter_t const& filter = **std::get_if<filter_t*>(&flag.destination);
    return filter.text.empty() ? "none" : std::string(filter.text);
}

/** Prints every flag as "- Label: value" in the grammar it parses from, labelled like "--time-limit" as "Time limit",
 *  skipping a flag that a later one of the same name shadows. */
inline void print_flags(std::span<flag_t const> flags) {
    for (std::size_t index = 0; index != flags.size(); ++index) {
        flag_t const& flag = flags[index];
        auto const shadows = [&](flag_t const& later) noexcept { return later.name == flag.name; };
        if (std::any_of(flags.begin() + index + 1, flags.end(), shadows)) continue;
        std::string label {flag.label.empty() ? flag.name.substr(2) : flag.label};
        for (char& character : label)
            if (character == '-' && flag.label.empty()) character = ' ';
        label.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(label.front())));
        fmt::println("- {}: {}", label, spell_value_(flag));
    }
}

#pragma endregion Arguments

#pragma region Engines

/** The names @c --isolation accepts, in the order of @c ustore_isolation_t. */
inline constexpr std::array<std::string_view, 2> isolation_names_k {"strict-serializable", "snapshot"};

/** The names @c --engine-transport accepts, in the order of @c ustore_transport_t. */
inline constexpr std::array<std::string_view, 5> transport_names_k {"automatic", "blocking", "linux-uring", "linux-aio",
                                                                    "linux-spdk"};

/** The engine a binary opens and the options its flags give the engine's init call. */
struct engine_options_t {

    /** The engine's name: @c ram, @c rocksdb, @c leveldb or @c nvme. */
    std::string_view name = "ram";

    /** The null-terminated database directory, or null for an in-memory database. */
    char const* directory = nullptr;

    /** How the transactions of @c ram see each other. */
    choice_t isolation {isolation_names_k};

    /** How @c nvme moves bytes between its files and memory. */
    choice_t transport {transport_names_k};
};

/** Opens the engine @p options name into @p database; the one place binaries tell the engines apart. */
inline void open_engine(engine_options_t const& options, ustore_database_t* database, ustore_error_t* error) noexcept {
    if (options.name == "ram") {
        ustore_database_init_ram_t init {.db = database,
                                         .error = error,
                                         .directory = options.directory,
                                         .isolation = static_cast<ustore_isolation_t>(options.isolation.selected)};
        return ustore_database_init_ram(&init);
    }
    if (options.name == "rocksdb") {
        ustore_database_init_rocksdb_t init {.db = database, .error = error, .directory = options.directory};
        return ustore_database_init_rocksdb(&init);
    }
    if (options.name == "leveldb") {
        ustore_database_init_leveldb_t init {.db = database, .error = error, .directory = options.directory};
        return ustore_database_init_leveldb(&init);
    }
    if (options.name == "nvme") {
        ustore_database_init_nvme_t init {.db = database,
                                          .error = error,
                                          .directory = options.directory,
                                          .transport = static_cast<ustore_transport_t>(options.transport.selected)};
        return ustore_database_init_nvme(&init);
    }
    *error = {ustore_error_invalid_argument_k, "No engine has that name"};
}

#pragma endregion Engines

} // namespace unum::ustore::tools
