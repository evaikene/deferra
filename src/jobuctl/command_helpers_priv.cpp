#include "command_helpers_priv.hpp"

#include <fmt/format.h>

#include <charconv>
#include <system_error>
#include <utility>

namespace jb::jobuctl::detail {

using namespace jb::core;
using namespace jb::jobu;

auto parse_failure(std::string message) -> CommandBuildResult
{
    return {.command = std::nullopt, .error = std::move(message)};
}

auto option_value(CommandLineArgument const& argument) -> std::optional<std::string_view>
{
    if (argument.kind() != CommandLineArgumentKind::Option || !argument.known() || argument.missing_value() ||
        !argument.value() || argument.value()->empty()) {
        return std::nullopt;
    }
    return argument.value();
}

auto parse_unsigned(std::string_view text, std::uint64_t minimum, std::uint64_t maximum) -> std::optional<std::uint64_t>
{
    auto value  = std::uint64_t{};
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value < minimum || value > maximum) {
        return std::nullopt;
    }
    return value;
}

auto is_output_command(CommandKind kind) -> bool
{
    return kind == CommandKind::AttemptOutput;
}

auto output_channel_matches(JobType type, OutputChannel channel) -> bool
{
    switch (type) {
        case JobType::Cli:
            return channel == OutputChannel::Stdout || channel == OutputChannel::Stderr;
        case JobType::Http:
            return channel == OutputChannel::Body || channel == OutputChannel::Headers;
    }
    return false;
}

auto append_output_options(JsonValue::Object&                   parameters,
                           std::span<CommandLineArgument const> arguments,
                           std::string_view                     command_name) -> std::optional<std::string>
{
    // The command registry has already removed delivery flags and rejected unknown or duplicate options.
    for (auto const& argument : arguments) {
        auto value = option_value(argument);
        if (!value) {
            return fmt::format("{} requires nonempty option values", command_name);
        }
        auto const name = argument.name();
        if (name == "offset" || name == "limit") {
            auto maximum = name == "offset" ? UINT64_MAX : std::uint64_t{65536};
            auto number  = parse_unsigned(*value, name == "offset" ? 0 : 1, maximum);
            if (!number) {
                return name == "offset" ? "--offset must be a nonnegative integer"
                                        : "--limit must be from 1 through 65536";
            }
            parameters.emplace(std::string{name}, JsonValue{.data = *number});
        }
        else {
            parameters.emplace("channel", JsonValue{.data = std::string{*value}});
        }
    }
    return std::nullopt;
}

} // namespace jb::jobuctl::detail
