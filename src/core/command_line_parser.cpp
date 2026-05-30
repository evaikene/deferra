#include "command_line_parser.hpp"

#include <fmt/format.h>

namespace jb::core {

namespace {

auto missing_value_error(std::string_view type, std::string_view token) -> std::string
{
    return fmt::format("missing {} value for argument: '{}'", type, token);
}

auto is_option_like(std::string_view token) -> bool
{
    return token.size() > 1 && token.front() == '-';
}

auto basename(std::string_view path) -> std::string_view
{
    auto const pos = path.find_last_of("/\\");
    return pos == std::string_view::npos ? path : path.substr(pos + 1);
}

} // anonymous namespace

CommandLineArgument::CommandLineArgument(CommandLineArgumentKind kind, std::string_view token)
    : _kind(kind)
    , _token(token)
{}

auto CommandLineArgument::value() const -> std::optional<std::string_view>
{
    return _value;
}

auto CommandLineArgument::missing_value() const -> bool
{
    return _value_mode == CommandLineValueMode::Required && !_value.has_value();
}

auto CommandLineArgument::boolean_value() const -> ValueResult<bool>
{
    if (_kind == CommandLineArgumentKind::Option || _kind == CommandLineArgumentKind::Unknown) {
        if (missing_value()) {
            return {.value = std::nullopt, .error = missing_value_error("boolean", _token)};
        }
        if (!_value) {
            return {.value = true, .error = {}};
        }
        return {.value = parse_boolean(*_value), .error = {}};
    }

    return {.value = parse_boolean(_token), .error = {}};
}

auto CommandLineArgument::integer_value() const -> ValueResult<long long>
{
    auto const text = conversion_value("integer");
    return text ? parse_integer(*text.value) : ValueResult<long long>{.value = std::nullopt, .error = text.error};
}

auto CommandLineArgument::floating_point_value() const -> ValueResult<double>
{
    auto const text = conversion_value("floating point");
    return text ? parse_floating_point(*text.value) : ValueResult<double>{.value = std::nullopt, .error = text.error};
}

auto CommandLineArgument::duration_value() const -> ValueResult<Duration>
{
    auto const text = conversion_value("duration");
    return text ? parse_duration(*text.value) : ValueResult<Duration>{.value = std::nullopt, .error = text.error};
}

auto CommandLineArgument::conversion_value(std::string_view type) const -> ValueResult<std::string_view>
{
    if (_kind == CommandLineArgumentKind::Positional) {
        return {.value = _token, .error = {}};
    }
    if (_value) {
        return {.value = _value, .error = {}};
    }
    return {.value = std::nullopt, .error = missing_value_error(type, _token)};
}

CommandLineParser::CommandLineParser(int argc, char const* const argv[])
    : _program_path(argc > 0 && argv && argv[0] ? argv[0] : "")
{
    parse(argc, argv);
}

CommandLineParser::CommandLineParser(int argc, char const* const argv[], std::span<CommandLineOption const> options)
    : _options(options)
    , _program_path(argc > 0 && argv && argv[0] ? argv[0] : "")
{
    parse(argc, argv);
}

auto CommandLineParser::program_name() const -> std::string_view
{
    return basename(_program_path);
}

auto CommandLineParser::find_long_option(std::string_view name) const -> CommandLineOption const*
{
    for (auto const& option : _options) {
        if (!option.long_name.empty() && option.long_name == name) {
            return &option;
        }
    }
    return nullptr;
}

auto CommandLineParser::find_short_option(char name) const -> CommandLineOption const*
{
    for (auto const& option : _options) {
        if (option.short_name == name) {
            return &option;
        }
    }
    return nullptr;
}

void CommandLineParser::parse(int argc, char const* const argv[])
{
    auto positional_only = false;

    for (int index = 1; index < argc; ++index) {
        auto const token = std::string_view{argv[index] ? argv[index] : ""};

        if (positional_only) {
            parse_positional(token);
            continue;
        }

        if (token == "--") {
            _arguments.push_back(CommandLineArgument{CommandLineArgumentKind::Terminator, token});
            positional_only = true;
            continue;
        }

        if (token.size() > 2 && token.starts_with("--")) {
            parse_long_option(argc, argv, index, token);
            continue;
        }

        if (is_option_like(token)) {
            parse_short_options(argc, argv, index, token);
            continue;
        }

        parse_positional(token);
    }
}

void CommandLineParser::parse_long_option(int argc, char const* const argv[], int& index, std::string_view token)
{
    auto argument = CommandLineArgument{CommandLineArgumentKind::Option, token};

    auto       name       = token.substr(2);
    auto const eq         = name.find('=');
    auto       inline_arg = eq != std::string_view::npos;
    if (inline_arg) {
        argument._value        = name.substr(eq + 1);
        argument._value_inline = true;
        name                   = name.substr(0, eq);
    }

    argument._name = name;
    apply_descriptor(argument, find_long_option(name));

    if (!argument._value && argument._value_mode == CommandLineValueMode::Required && index + 1 < argc) {
        auto const next = std::string_view{argv[index + 1] ? argv[index + 1] : ""};
        if (!is_option_like(next)) {
            ++index;
            argument._value = next;
        }
    }

    _arguments.emplace_back(argument);
}

void CommandLineParser::parse_short_options(int argc, char const* const argv[], int& index, std::string_view token)
{
    for (size_t offset = 1; offset < token.size(); ++offset) {
        auto argument        = CommandLineArgument{CommandLineArgumentKind::Option, token};
        argument._short_name = token[offset];
        argument._name       = token.substr(offset, 1);
        auto const* option   = find_short_option(token[offset]);
        apply_descriptor(argument, option);

        if (argument._value_mode == CommandLineValueMode::Required ||
            argument._value_mode == CommandLineValueMode::Optional) {
            if (offset + 1 < token.size()) {
                argument._value        = token.substr(offset + 1);
                argument._value_inline = true;
                _arguments.emplace_back(argument);
                return;
            }

            if (argument._value_mode == CommandLineValueMode::Required && index + 1 < argc) {
                auto const next = std::string_view{argv[index + 1] ? argv[index + 1] : ""};
                if (!is_option_like(next)) {
                    ++index;
                    argument._value = next;
                }
            }

            _arguments.emplace_back(argument);
            return;
        }

        _arguments.emplace_back(argument);
    }
}

void CommandLineParser::parse_positional(std::string_view token)
{
    _arguments.emplace_back(CommandLineArgument{CommandLineArgumentKind::Positional, token});
}

void CommandLineParser::apply_descriptor(CommandLineArgument& argument, CommandLineOption const* option)
{
    if (!option) {
        argument._kind = CommandLineArgumentKind::Unknown;
        return;
    }

    argument._known      = true;
    argument._value_mode = option->value_mode;
    if (!option->long_name.empty()) {
        argument._name = option->long_name;
    }
    if (option->short_name != '\0') {
        argument._short_name = option->short_name;
    }
}

} // namespace jb::core
