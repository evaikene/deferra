#include "commands_priv.hpp"

#include "command_helpers_priv.hpp"

#include <fmt/format.h>

#include <cstdio> // IWYU pragma: keep for stdout/stderr macros
#include <utility>

namespace jb::jobuctl::detail {

using namespace jb::core;
using namespace jb::jobu;

auto parse_system_command(std::filesystem::path                socket_path,
                          std::string_view                     action,
                          std::span<CommandLineArgument const> arguments) -> ParseResult
{
    if (action != "info" || !arguments.empty()) {
        return parse_failure("unknown command");
    }
    return {.command = Command{.socket_path = std::move(socket_path)}, .error = {}};
}

void print_system_info(SystemInfo const& info)
{
    fmt::print(stdout, "Daemon version: {}\n", info.daemon_version);
    fmt::print(stdout, "API version: {}.{}\n", info.api_version.major, info.api_version.minor);
    fmt::print(stdout, "Capabilities:\n");
    for (auto const& capability : info.capabilities) {
        fmt::print(stdout, "  {}\n", capability);
    }
}

} // namespace jb::jobuctl::detail
