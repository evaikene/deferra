#include "output_priv.hpp"

#include "commands/commands_priv.hpp"
#include "management_json.hpp"
#include "system_info.hpp"

#include "json.hpp"

#include <fmt/format.h>

#include <cstdint>
#include <cstdio> // IWYU pragma: keep for stdout/stderr macros
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace jb::jobuctl::detail {

using namespace jb::core;
using namespace jb::jobu;
using namespace jb::rpc;

namespace {

auto category_text(ErrorCategory category) -> std::string_view
{
    switch (category) {
        case ErrorCategory::InvalidArgument:
            return "invalid_argument";
        case ErrorCategory::NotFound:
            return "not_found";
        case ErrorCategory::Conflict:
            return "conflict";
        case ErrorCategory::PermissionDenied:
            return "permission_denied";
        case ErrorCategory::Unavailable:
            return "unavailable";
        case ErrorCategory::ResourceExhausted:
            return "resource_exhausted";
        case ErrorCategory::Cancelled:
            return "cancelled";
        case ErrorCategory::Timeout:
            return "timeout";
        case ErrorCategory::Io:
            return "io";
        case ErrorCategory::Unsupported:
            return "unsupported";
        case ErrorCategory::Internal:
            return "internal";
    }
    return "internal";
}

auto nullable_string(std::optional<std::string> const& value) -> JsonValue
{
    return value ? JsonValue{.data = *value} : JsonValue{};
}

auto nullable_number(std::optional<std::int64_t> value) -> JsonValue
{
    return value ? JsonValue{.data = *value} : JsonValue{};
}

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

auto encoded_reply(Command const& command, ControlReply const& reply, AttributeRegistry const& registry)
    -> Result<JsonValue, Error>
{
    if (command.kind == CommandKind::SystemInfo) {
        if (auto const* info = std::get_if<SystemInfo>(&reply)) {
            return Result<JsonValue, Error>::success(system_info_to_json(*info));
        }
    }
    else if (command.kind == CommandKind::QueueList) {
        if (auto const* page = std::get_if<QueuePage>(&reply)) {
            return queue_page_to_json(*page, registry);
        }
    }
    else if (command.kind == CommandKind::JobList) {
        if (auto const* page = std::get_if<JobPage>(&reply)) {
            return job_page_to_json(*page, registry);
        }
    }
    else if (command.kind == CommandKind::QueueDelete || command.kind == CommandKind::JobDelete) {
        if (std::holds_alternative<EmptyReply>(reply)) {
            return Result<JsonValue, Error>::success(JsonValue{});
        }
    }
    else if (is_job_command(command.kind)) {
        if (auto const* job = std::get_if<JobDefinition>(&reply)) {
            return job_to_json(*job, registry);
        }
    }
    else if (auto const* queue = std::get_if<Queue>(&reply)) {
        return queue_to_json(*queue, registry);
    }
    return Result<JsonValue, Error>::failure({
        .category = ErrorCategory::Internal,
        .code     = "jobuctl.output.invalid_reply",
        .message  = "Unable to render the daemon reply",
    });
}

} // namespace

auto escape_human(std::string_view value) -> std::string
{
    auto escaped = std::string{};
    escaped.reserve(value.size());
    for (auto index = std::size_t{0}; index < value.size(); ++index) {
        auto const byte = static_cast<unsigned char>(value[index]);
        if (byte == '\n') {
            escaped += "\\n";
        }
        else if (byte == '\r') {
            escaped += "\\r";
        }
        else if (byte == '\t') {
            escaped += "\\t";
        }
        else if (byte < 0x20U || byte == 0x7fU) {
            escaped += fmt::format("\\x{:02X}", byte);
        }
        else if (byte == 0xc2U && index + 1U < value.size() && static_cast<unsigned char>(value[index + 1U]) >= 0x80U &&
                 static_cast<unsigned char>(value[index + 1U]) <= 0x9fU) {
            escaped += fmt::format("\\u00{:02X}", static_cast<unsigned char>(value[++index]));
        }
        else {
            escaped += value[index];
        }
    }
    return escaped;
}

auto local_error(Error const& error, bool outcome_unknown) -> CliError
{
    return {
        .kind            = "local",
        .code            = error.code,
        .category        = std::string{category_text(error.category)},
        .message         = error.message,
        .outcome_unknown = outcome_unknown,
    };
}

auto remote_error(RpcError const& error) -> CliError
{
    auto rendered = CliError{
        .kind     = "remote",
        .code     = "jobuctl.remote.rpc_error",
        .rpc_code = error.code,
        .message  = error.message,
    };
    if (error.code == static_cast<std::int64_t>(ErrorCode::ApplicationError) && error.data && error.data->is_object()) {
        auto const& fields   = error.data->as_object();
        auto const  code     = fields.find("code");
        auto const  category = fields.find("category");
        if (code != fields.end() && code->second.is_string()) {
            rendered.code = code->second.as_string();
        }
        if (category != fields.end() && category->second.is_string()) {
            rendered.category = category->second.as_string();
        }
    }
    return rendered;
}

void print_error(bool json, CliError const& error)
{
    if (!json) {
        if (error.kind == "remote" && error.code != "jobuctl.remote.rpc_error") {
            fmt::print(stderr, "jobuctl: {} ({})\n", escape_human(error.message), escape_human(error.code));
        }
        else if (error.kind == "remote") {
            fmt::print(stderr, "jobuctl: remote RPC error {}: {}\n", *error.rpc_code, escape_human(error.message));
        }
        else if (error.outcome_unknown) {
            fmt::print(stderr, "jobuctl: {} (mutation outcome unknown)\n", escape_human(error.message));
        }
        else {
            fmt::print(stderr, "jobuctl: {}\n", escape_human(error.message));
        }
        return;
    }

    auto fields = JsonValue::Object{
        {"kind",            JsonValue{.data = error.kind}           },
        {"code",            JsonValue{.data = error.code}           },
        {"rpc_code",        nullable_number(error.rpc_code)         },
        {"category",        nullable_string(error.category)         },
        {"message",         JsonValue{.data = error.message}        },
        {"outcome_unknown", JsonValue{.data = error.outcome_unknown}},
    };
    auto value      = JsonValue{.data = JsonValue::Object{{"error", JsonValue{.data = std::move(fields)}}}};
    auto serialized = serialize_json(value);
    if (serialized) {
        fmt::print(stderr, "{}\n", *serialized);
    }
    else {
        fmt::print(stderr,
                   "{{\"error\":{{\"kind\":\"local\",\"code\":\"jobuctl.output.failed\",\"rpc_code\":null,\"category\":"
                   "\"internal\",\"message\":\"Unable to render error\",\"outcome_unknown\":false}}}}\n");
    }
}

auto print_command_result(Command const& command, ControlReply const& reply, StandardAttributeRegistry const& registry)
    -> bool
{
    if (command.json) {
        auto value = encoded_reply(command, reply, registry);
        if (!value) {
            return false;
        }
        auto serialized = serialize_json(*value);
        if (!serialized) {
            return false;
        }
        fmt::print(stdout, "{}\n", *serialized);
        return true;
    }

    if (command.kind == CommandKind::SystemInfo) {
        auto const* info = std::get_if<SystemInfo>(&reply);
        if (!info) {
            return false;
        }
        print_system_info(*info);
        return true;
    }
    if (is_job_command(command.kind)) {
        return print_job_result(command, reply);
    }
    return print_queue_result(command, reply);
}

} // namespace jb::jobuctl::detail
