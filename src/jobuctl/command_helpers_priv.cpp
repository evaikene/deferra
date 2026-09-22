#include "command_helpers_priv.hpp"

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

} // namespace jb::jobuctl::detail
