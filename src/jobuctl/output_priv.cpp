#include "output_priv.hpp"

#include "commands/commands_priv.hpp"
#include "control_json.hpp"
#include "history_json.hpp"
#include "management_json.hpp"
#include "secret_json.hpp"
#include "statistics_json.hpp"
#include "system_info.hpp"

#include "json.hpp"

#include <fmt/format.h>

#include <cerrno>
#include <cstdint>
#include <cstdio> // IWYU pragma: keep for stdout/stderr macros
#include <fcntl.h>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
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
        case CommandKind::JobRunNow:
        case CommandKind::RunGet:
        case CommandKind::RunList:
        case CommandKind::RunCancel:
        case CommandKind::AttemptGet:
        case CommandKind::AttemptList:
        case CommandKind::AttemptOutput:
        case CommandKind::SecretSet:
        case CommandKind::SecretList:
        case CommandKind::SecretDelete:
        case CommandKind::SystemStats:
        case CommandKind::QueueStats:
        case CommandKind::ScheduleValidate:
        case CommandKind::ScheduleNext:
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
    else if (command.kind == CommandKind::JobRunNow || command.kind == CommandKind::RunGet) {
        if (auto const* run = std::get_if<RunDetails>(&reply)) {
            return run_details_to_json(*run, registry);
        }
    }
    else if (command.kind == CommandKind::RunList) {
        if (auto const* page = std::get_if<RunPage>(&reply)) {
            return run_page_to_json(*page);
        }
    }
    else if (command.kind == CommandKind::RunCancel) {
        if (auto const* result = std::get_if<CancelRunResult>(&reply)) {
            return cancel_run_result_to_json(*result, registry);
        }
    }
    else if (command.kind == CommandKind::AttemptGet) {
        if (auto const* attempt = std::get_if<AttemptDetails>(&reply)) {
            return attempt_details_to_json(*attempt);
        }
    }
    else if (command.kind == CommandKind::AttemptList) {
        if (auto const* page = std::get_if<AttemptPage>(&reply)) {
            return attempt_page_to_json(*page);
        }
    }
    else if (command.kind == CommandKind::AttemptOutput) {
        if (auto const* chunk = std::get_if<AttemptOutputChunk>(&reply)) {
            return attempt_output_chunk_to_json(*chunk);
        }
    }
    else if (command.kind == CommandKind::SecretSet) {
        if (auto const* metadata = std::get_if<SecretMetadata>(&reply)) {
            return secret_metadata_to_json(*metadata);
        }
    }
    else if (command.kind == CommandKind::SecretList) {
        if (auto const* page = std::get_if<SecretPage>(&reply)) {
            return secret_page_to_json(*page);
        }
    }
    else if (command.kind == CommandKind::SecretDelete) {
        if (std::holds_alternative<EmptyReply>(reply)) {
            return Result<JsonValue, Error>::success(JsonValue{});
        }
    }
    else if (command.kind == CommandKind::SystemStats || command.kind == CommandKind::QueueStats) {
        if (auto const* page = std::get_if<StatisticsPage>(&reply)) {
            return statistics_page_to_json(*page);
        }
    }
    else if (command.kind == CommandKind::ScheduleValidate) {
        if (std::holds_alternative<ScheduleValidationReply>(reply)) {
            return Result<JsonValue, Error>::success(schedule_validate_result_to_json());
        }
    }
    else if (command.kind == CommandKind::ScheduleNext) {
        if (auto const* result = std::get_if<ScheduleNextReply>(&reply)) {
            return schedule_next_result_to_json(result->occurrences);
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
    if (command.kind == CommandKind::SystemStats || command.kind == CommandKind::QueueStats) {
        return print_statistics_result(reply);
    }
    if (command.kind == CommandKind::ScheduleValidate || command.kind == CommandKind::ScheduleNext) {
        return print_schedule_result(command.kind, reply);
    }
    if (is_job_command(command.kind)) {
        return print_job_result(command, reply);
    }
    if (command.kind == CommandKind::JobRunNow || command.kind == CommandKind::RunGet ||
        command.kind == CommandKind::RunList || command.kind == CommandKind::RunCancel) {
        return print_run_result(command, reply, registry);
    }
    if (command.kind == CommandKind::AttemptGet || command.kind == CommandKind::AttemptList ||
        command.kind == CommandKind::AttemptOutput) {
        return print_attempt_result(command, reply);
    }
    if (command.kind == CommandKind::SecretSet || command.kind == CommandKind::SecretList ||
        command.kind == CommandKind::SecretDelete) {
        return print_secret_result(command, reply);
    }
    return print_queue_result(command, reply);
}

auto write_output_chunk(Command const& command, AttemptOutputChunk const& chunk) -> Result<void, Error>
{
    auto const failure = [](std::string code, std::string message) {
        return Result<void, Error>::failure(
            {.category = ErrorCategory::InvalidArgument, .code = std::move(code), .message = std::move(message)});
    };
    if (command.kind != CommandKind::AttemptOutput || (!command.raw && !command.output_file)) {
        return failure("jobuctl.output.invalid_mode", "Output delivery mode is invalid");
    }

    auto const* path = command.output_file ? &*command.output_file : nullptr;
    auto const  fd   = path ? ::open(path->c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600) : STDOUT_FILENO;
    if (fd < 0) {
        return failure("jobuctl.output.open_failed", "Unable to create output file exclusively");
    }

    // The typed chunk owns decoded raw bytes, even when the wire representation was base64.
    auto const bytes   = jb::core::as_string_view(chunk.data);
    auto       written = std::size_t{0};
    while (written < bytes.size()) {
        auto const count = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            if (path) {
                static_cast<void>(::close(fd));
                static_cast<void>(::unlink(path->c_str()));
            }
            return failure("jobuctl.output.write_failed", "Unable to write output chunk");
        }
        written += static_cast<std::size_t>(count);
    }
    if (path && ::close(fd) != 0) {
        static_cast<void>(::unlink(path->c_str()));
        return failure("jobuctl.output.write_failed", "Unable to finish output file");
    }
    return Result<void, Error>::success();
}

} // namespace jb::jobuctl::detail
