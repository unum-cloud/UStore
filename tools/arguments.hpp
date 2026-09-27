/**
 *  @file tools/arguments.hpp
 *  @author Ash Vardanian
 *  @date September 15, 2026
 *  @brief Command-line flags for every UStore binary: one table parses the arguments and prints the usage.
 *
 *  Flags are long kebab-case words whose value follows as the next argument or after @c =. Sizes carry IEC
 *  suffixes, durations carry @c ms, @c s, @c min or @c h, counts are plain integers, and lists are comma-separated.
 *  A text value views the tail of its @c argv entry, so it is null-terminated; list items are not.
 *
 *  @code
 *  bytes_t memory_limit {64 << 20};
 *  std::string_view directory;
 *  std::array<std::string_view, 2> const formats {"ndjson", "csv"};
 *  choice_t format {formats};
 *  std::array<flag_t, 3> const flags {{
 *      {"--directory", "path", "database directory", &directory},
 *      {"--memory-limit", "size", "arena budget, like 64MiB", &memory_limit},
 *      {"--format", "name", "export format", &format},
 *  }};
 *  arguments_t const parsed = parse_arguments(argc, argv, flags, {});
 *  @endcode
 */
#pragma once

#include <cstddef> // `std::size_t`
#include <cstdint> // `std::uint64_t`

#include <array>        // `std::array`
#include <charconv>     // `std::from_chars`
#include <chrono>       // `std::chrono::nanoseconds`
#include <limits>       // `std::numeric_limits`
#include <span>         // `std::span`
#include <string_view>  // `std::string_view`
#include <system_error> // `std::errc`
#include <variant>      // `std::variant`

#include <fmt/format.h>
#include <fmt/ranges.h> // `fmt::join`

#include <ustore/db.h> // `ustore_database_init_t` and the init call of every engine

namespace unum::ustore {

/** Outcome of parsing one value or a whole argument list. */
enum class argument_status_t {
    parsed_k,           ///< Every argument matched its flag.
    help_k,             ///< `--help` was given, so the caller prints the usage and exits.
    unknown_flag_k,     ///< An argument starting with `--` names no flag.
    missing_value_k,    ///< A flag taking a value ended the argument list.
    unexpected_value_k, ///< A switch was given a value through `=`.
    invalid_number_k,   ///< The digits do not form a non-negative integer.
    invalid_unit_k,     ///< The suffix names no unit of the flag's kind.
    invalid_choice_k,   ///< The value names none of the flag's choices.
    out_of_range_k,     ///< The value overflows its destination or falls outside its bounds.
    too_many_items_k,   ///< A list or the positional arguments exceed their capacity.
    too_few_items_k,    ///< A list holds fewer items than it requires.
};

/** A size in bytes, written @c 64MiB or @c 1GiB; a bare number is bytes. */
struct bytes_t {
    std::size_t value = 0;
};

/** A plain non-negative integer, accepted only within @b [minimum,maximum]. */
struct count_t {
    std::size_t value = 0;
    std::size_t minimum = 0;
    std::size_t maximum = std::numeric_limits<std::size_t>::max();
};

/** Comma-separated counts like @c 16,256,4096, each within @b [minimum,maximum].
 *  Between @c minimum_size and @c maximum_size of them are accepted. */
struct counts_t {
    static constexpr std::size_t capacity_k = 16;
    std::array<std::size_t, capacity_k> values {};
    std::size_t size = 0;
    std::size_t minimum = 0;
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

/** Whether a flag without a value appeared. */
enum class switch_t : bool {
    absent_k,
    present_k,
};

/** One flag: its name with both dashes, the usage placeholder, one help line, and the value it writes. */
struct flag_t {
    std::string_view name;
    std::string_view placeholder;
    std::string_view help;
    std::variant<std::string_view*, count_t*, counts_t*, bytes_t*, std::chrono::nanoseconds*, texts_t*, choice_t*,
                 switch_t*>
        destination;
};

/** Result of @c parse_arguments: the status, the argument it concerns, and how many positionals were collected. */
struct arguments_t {
    argument_status_t status = argument_status_t::parsed_k;
    std::string_view argument;
    std::size_t positionals_count = 0;
};

#pragma region Values

/** A suffix and the multiplier it applies to the digits before it. */
struct unit_t {
    std::string_view suffix;
    std::uint64_t multiplier = 1;
};

inline constexpr std::array<unit_t, 5> byte_units_k {{
    {"", 1},
    {"KiB", std::uint64_t {1} << 10},
    {"MiB", std::uint64_t {1} << 20},
    {"GiB", std::uint64_t {1} << 30},
    {"TiB", std::uint64_t {1} << 40},
}};

inline constexpr std::array<unit_t, 4> duration_units_k {{
    {"ms", 1'000'000},
    {"s", 1'000'000'000},
    {"min", 60'000'000'000},
    {"h", 3'600'000'000'000},
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

/** Parses a size such as @c 64MiB into bytes. */
[[nodiscard]] inline argument_status_t parse_bytes(std::string_view text, bytes_t& bytes) noexcept {
    std::uint64_t value = 0;
    argument_status_t const status = parse_with_units_(text, byte_units_k, value);
    if (status != argument_status_t::parsed_k) return status;
    if (value > std::numeric_limits<std::size_t>::max()) return argument_status_t::out_of_range_k;
    bytes.value = static_cast<std::size_t>(value);
    return argument_status_t::parsed_k;
}

/** Parses a duration such as @c 500ms or @c 10s; a bare number is rejected. */
[[nodiscard]] inline argument_status_t parse_duration(std::string_view text,
                                                      std::chrono::nanoseconds& duration) noexcept {
    std::uint64_t value = 0;
    argument_status_t const status = parse_with_units_(text, duration_units_k, value);
    if (status != argument_status_t::parsed_k) return status;
    if (value > static_cast<std::uint64_t>(std::numeric_limits<std::chrono::nanoseconds::rep>::max()))
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
        if (result.status != argument_status_t::parsed_k) return result;
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

#pragma endregion Arguments

/** An engine's init call of the C ABI. */
using database_init_t = void (*)(ustore_database_init_t*);

/** The init call of the engine @p name names, or null for one this build lacks; the one place binaries tell the engines
 *  apart. */
[[nodiscard]] inline database_init_t database_init_of(std::string_view name) noexcept {
    if (name == "ram") return ustore_database_init_ram;
#if USTORE_WITH_ROCKSDB
    if (name == "rocksdb") return ustore_database_init_rocksdb;
#endif
#if USTORE_WITH_LEVELDB
    if (name == "leveldb") return ustore_database_init_leveldb;
#endif
#if USTORE_WITH_NVME
    if (name == "nvme") return ustore_database_init_nvme;
#endif
    return nullptr;
}

} // namespace unum::ustore
