#pragma once

#include "utils.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace jb::core {

/// Describes how an option accepts values.
enum class CommandLineValueMode : std::uint8_t {
    None,
    Required,
    Optional,
};

/// Compile-time friendly command-line option descriptor.
struct CommandLineOption {
    std::string_view     long_name;
    char                 short_name = '\0';
    CommandLineValueMode value_mode = CommandLineValueMode::None;
};

/// Parsed command-line argument classification.
enum class CommandLineArgumentKind : std::uint8_t {
    Unknown,
    Option,
    Positional,
    Terminator,
};

/// A parsed command-line argument in its original command-line order.
///
/// String views returned by this class point into the original `argv` storage
/// or into option descriptor storage supplied to `CommandLineParser`.
class CommandLineArgument {
public:

    /// Returns the argument kind.
    auto kind() const -> CommandLineArgumentKind { return _kind; }

    /// Returns the original token text from argv.
    auto token() const -> std::string_view { return _token; }

    /// Returns the long option name without leading dashes.
    auto name() const -> std::string_view { return _name; }

    /// Returns the short option character, or '\0' when this is not a short option.
    auto short_name() const -> char { return _short_name; }

    /// Returns true when this option matched one of the supplied descriptors.
    auto known() const -> bool { return _known; }

    /// Returns true when this option descriptor requires or accepts a value.
    auto expects_value() const -> bool { return _value_mode != CommandLineValueMode::None; }

    /// Returns the descriptor value mode for this argument.
    auto value_mode() const -> CommandLineValueMode { return _value_mode; }

    /// Returns true when the argument has a value, including an empty inline value.
    auto has_value() const -> bool { return _value.has_value(); }

    /// Returns the parsed value, if present.
    auto value() const -> std::optional<std::string_view>;

    /// Returns true when a required value was not present.
    auto missing_value() const -> bool;

    /// Returns true when the value came from the same token, such as `--name=value` or `-ovalue`.
    auto value_is_inline() const -> bool { return _value_inline; }

    /// Returns the argument value converted to a boolean.
    ///
    /// Positional arguments convert their token. Options with a value convert the value.
    /// Options without a value are treated as boolean flags and return true.
    auto boolean_value() const -> ValueResult<bool>;

    /// Returns the argument value converted to a signed integer.
    ///
    /// Positional arguments convert their token. Options convert their value and fail when no value is present.
    auto integer_value() const -> ValueResult<long long>;

    /// Returns the argument value converted to a finite floating point number.
    ///
    /// Positional arguments convert their token. Options convert their value and fail when no value is present.
    auto floating_point_value() const -> ValueResult<double>;

    /// Returns the argument value converted to a time interval.
    ///
    /// Positional arguments convert their token. Options convert their value and fail when no value is present.
    auto duration_value() const -> ValueResult<Duration>;

private:

    friend class CommandLineParser;

    CommandLineArgument(CommandLineArgumentKind kind, std::string_view token);

    auto conversion_value(std::string_view type) const -> ValueResult<std::string_view>;

    CommandLineArgumentKind         _kind = CommandLineArgumentKind::Positional;
    std::string_view                _token;
    std::string_view                _name;
    char                            _short_name = '\0';
    bool                            _known      = false;
    CommandLineValueMode            _value_mode = CommandLineValueMode::None;
    std::optional<std::string_view> _value;
    bool                            _value_inline = false;
};

/// Parses command-line arguments into an ordered argument stream.
///
/// Parsing is intentionally non-validating: unknown options and missing values
/// are represented in the argument stream so callers can decide how to handle them.
/// Parsed arguments contain non-owning views into `argv` and the supplied option
/// descriptors, so both must outlive the parser.
class CommandLineParser {
public:

    /// Parses `argv[1..argc)` without option descriptors.
    explicit CommandLineParser(int argc, char const* const argv[]);

    /// Parses `argv[1..argc)` using compile-time friendly option descriptors.
    CommandLineParser(int argc, char const* const argv[], std::span<CommandLineOption const> options);

    /// Returns the executable name from `argv[0]`, without POSIX or Windows path components.
    auto program_name() const -> std::string_view;

    /// Returns all parsed arguments in their original command-line order.
    auto arguments() const -> std::vector<CommandLineArgument> const& { return _arguments; }

    /// Returns an iterator to the first parsed argument.
    auto begin() const -> std::vector<CommandLineArgument>::const_iterator { return _arguments.begin(); }

    /// Returns an iterator one past the last parsed argument.
    auto end() const -> std::vector<CommandLineArgument>::const_iterator { return _arguments.end(); }

private:

    auto        find_long_option(std::string_view name) const -> CommandLineOption const*;
    auto        find_short_option(char name) const -> CommandLineOption const*;
    void        parse(int argc, char const* const argv[]);
    void        parse_long_option(int argc, char const* const argv[], int& index, std::string_view token);
    void        parse_short_options(int argc, char const* const argv[], int& index, std::string_view token);
    void        parse_positional(std::string_view token);
    static void apply_descriptor(CommandLineArgument& argument, CommandLineOption const* option);

    std::span<CommandLineOption const> _options;
    std::string_view                   _program_path;
    std::vector<CommandLineArgument>   _arguments;
};

} // namespace jb::core
