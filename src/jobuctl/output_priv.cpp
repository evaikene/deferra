#include "output_priv.hpp"

#include "commands/commands_priv.hpp"

#include <fmt/format.h>

#include <cstdint>
#include <cstdio> // IWYU pragma: keep for stdout/stderr macros
#include <optional>

namespace jb::jobuctl::detail {

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::rpc;

namespace {

auto is_job_command(CommandKind kind) noexcept -> bool
{
    switch (kind) {
        case CommandKind::JobCreate:
        case CommandKind::JobGet:
        case CommandKind::JobList:
        case CommandKind::JobUpdate:
        case CommandKind::JobSuspend:
        case CommandKind::JobResume:
        case CommandKind::JobMove:
        case CommandKind::JobDelete:
            return true;
        case CommandKind::SystemInfo:
        case CommandKind::QueueCreate:
        case CommandKind::QueueGet:
        case CommandKind::QueueList:
        case CommandKind::QueueUpdate:
        case CommandKind::QueueSuspend:
        case CommandKind::QueueResume:
        case CommandKind::QueueDelete:
            return false;
    }
    return false;
}

auto application_error_code(RpcError const& error) -> std::optional<std::string_view>
{
    // Only represented application errors carry a domain code; ordinary RPC errors keep their numeric identity.
    if (error.code != static_cast<std::int64_t>(ErrorCode::ApplicationError) || !error.data ||
        !error.data->is_object()) {
        return std::nullopt;
    }
    auto const& object = error.data->as_object();
    auto const  member = object.find("code");
    if (member == object.end() || !member->second.is_string()) {
        return std::nullopt;
    }
    return member->second.as_string();
}

} // namespace

void print_operator_error(std::string_view message)
{
    fmt::print(stderr, "jobuctl: {}\n", message);
}

void print_remote_error(RpcError const& error)
{
    if (auto const code = application_error_code(error)) {
        fmt::print(stderr, "jobuctl: {} ({})\n", error.message, *code);
    }
    else {
        fmt::print(stderr, "jobuctl: remote RPC error {}: {}\n", error.code, error.message);
    }
}

auto print_command_result(Command const& command, JsonValue const& value, StandardAttributeRegistry const& registry)
    -> bool
{
    if (is_job_command(command.kind)) {
        return print_job_result(command, value, registry);
    }
    return print_queue_result(command, value, registry);
}

} // namespace jb::jobuctl::detail
