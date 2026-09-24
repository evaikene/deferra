#include "commands_priv.hpp"

#include "command_helpers_priv.hpp"
#include "history_json.hpp"
#include "json.hpp"

#include <fmt/format.h>

#include <cstdio> // IWYU pragma: keep for stdout macro
#include <optional>
#include <string>
#include <utility>

namespace jb::jobuctl::detail {

using namespace jb::core;
using namespace jb::jobu;

namespace {

auto attempt_identity(std::span<CommandLineArgument const> arguments) -> std::optional<JsonValue::Object>
{
    if (arguments.size() < 2U || arguments[0].kind() != CommandLineArgumentKind::Positional ||
        arguments[1].kind() != CommandLineArgumentKind::Positional) {
        return std::nullopt;
    }
    auto number = parse_unsigned(arguments[1].token(), 1, UINT64_MAX);
    if (!number) {
        return std::nullopt;
    }
    return JsonValue::Object{
        {"run_id",         JsonValue{.data = std::string{arguments[0].token()}}},
        {"attempt_number", JsonValue{.data = *number}                          }
    };
}

auto parse_get(std::filesystem::path socket_path, std::span<CommandLineArgument const> arguments) -> CommandBuildResult
{
    auto identity = attempt_identity(arguments);
    if (!identity || arguments.size() != 2U) {
        return parse_failure("attempt get requires a run UUID and positive attempt number");
    }
    auto key = attempt_get_request_from_json(JsonValue{.data = std::move(*identity)});
    if (!key) {
        return parse_failure("attempt get identity is invalid");
    }
    return {
        .command = Command{.socket_path = std::move(socket_path),
                           .kind        = CommandKind::AttemptGet,
                           .request     = std::move(key).value()}
    };
}

auto parse_list(std::filesystem::path socket_path, std::span<CommandLineArgument const> arguments) -> CommandBuildResult
{
    auto params = JsonValue::Object{};
    for (auto const& argument : arguments) {
        if (argument.kind() == CommandLineArgumentKind::Positional) {
            params.emplace("run_id", JsonValue{.data = std::string{argument.token()}});
            continue;
        }
        auto value = option_value(argument);
        if (!value) {
            return parse_failure("attempt list requires nonempty option values");
        }
        if (argument.name() == "limit") {
            auto limit = parse_unsigned(*value, 1, 200);
            if (!limit) {
                return parse_failure("--limit must be from 1 through 200");
            }
            params.emplace("limit", JsonValue{.data = *limit});
        }
        else {
            params.emplace("cursor", JsonValue{.data = std::string{*value}});
        }
    }
    auto request = attempt_list_request_from_json(JsonValue{.data = std::move(params)});
    if (!request) {
        return parse_failure("attempt list requires a run UUID or cursor alone");
    }
    return {
        .command = Command{.socket_path = std::move(socket_path),
                           .kind        = CommandKind::AttemptList,
                           .request     = std::move(request).value()}
    };
}

auto parse_output(std::filesystem::path socket_path, std::span<CommandLineArgument const> arguments)
    -> CommandBuildResult
{
    auto identity = attempt_identity(arguments);
    if (!identity) {
        return parse_failure("attempt output requires a run UUID and positive attempt number");
    }
    for (auto const& argument : arguments.subspan(2)) {
        auto value = option_value(argument);
        if (!value) {
            return parse_failure("attempt output requires nonempty option values");
        }
        auto const name = argument.name();
        if (name == "offset" || name == "limit") {
            auto maximum = name == "offset" ? UINT64_MAX : std::uint64_t{65536};
            auto number  = parse_unsigned(*value, name == "offset" ? 0 : 1, maximum);
            if (!number) {
                return parse_failure(name == "offset" ? "--offset must be a nonnegative integer"
                                                      : "--limit must be from 1 through 65536");
            }
            identity->emplace(std::string{name}, JsonValue{.data = *number});
        }
        else {
            identity->emplace("channel", JsonValue{.data = std::string{*value}});
        }
    }
    auto request = attempt_output_request_from_json(JsonValue{.data = std::move(*identity)});
    if (!request) {
        return parse_failure("attempt output identity or channel is invalid");
    }
    return {
        .command = Command{.socket_path = std::move(socket_path),
                           .kind        = CommandKind::AttemptOutput,
                           .request     = std::move(request).value()}
    };
}

auto summary_line(AttemptSummary const& attempt) -> std::optional<std::string>
{
    auto encoded = attempt_summary_to_json(attempt);
    if (!encoded) {
        return std::nullopt;
    }
    auto const& fields    = encoded->as_object();
    auto        outcome   = fields.at("outcome").is_null() ? "pending" : fields.at("outcome").as_string();
    auto        started   = serialize_json(fields.at("started_at"));
    auto        completed = serialize_json(fields.at("completed_at"));
    if (!started || !completed) {
        return std::nullopt;
    }
    return fmt::format("Attempt {}:{}: state={}, outcome={}, due_at={}, started_at={}, completed_at={}\n",
                       attempt.run_id.to_string(),
                       attempt.attempt_number,
                       fields.at("state").as_string(),
                       outcome,
                       fields.at("due_at").as_string(),
                       *started,
                       *completed);
}

auto output_text(AttemptOutputChunk const& chunk) -> std::optional<std::string>
{
    auto encoded = attempt_output_chunk_to_json(chunk);
    if (!encoded) {
        return std::nullopt;
    }
    auto const& fields  = encoded->as_object();
    auto        total   = serialize_json(fields.at("total_bytes"));
    auto        omitted = serialize_json(fields.at("omitted_bytes"));
    auto        next    = serialize_json(fields.at("next_offset"));
    if (!total || !omitted || !next) {
        return std::nullopt;
    }
    auto text =
        fmt::format("Output {}:{} channel={}, status={}, offset={}, bytes_returned={}, retained_bytes={}, "
                    "total_bytes={}, omitted_bytes={}, next_offset={}, truncated={}, capture_lost={}, encoding={}\n",
                    chunk.attempt.run_id.to_string(),
                    chunk.attempt.attempt_number,
                    fields.at("channel").as_string(),
                    fields.at("status").as_string(),
                    chunk.offset,
                    chunk.bytes_returned,
                    chunk.retained_bytes,
                    *total,
                    *omitted,
                    *next,
                    chunk.truncated,
                    chunk.capture_lost,
                    fields.at("encoding").as_string());
    text += fmt::format("Data: {}\n", escape_human(fields.at("data").as_string()));
    return text;
}

} // namespace

auto parse_attempt_command(std::filesystem::path                socket_path,
                           std::string_view                     action,
                           std::span<CommandLineArgument const> arguments,
                           StandardAttributeRegistry const&     registry) -> CommandBuildResult
{
    static_cast<void>(registry);
    if (action == "get") {
        return parse_get(std::move(socket_path), arguments);
    }
    if (action == "list") {
        return parse_list(std::move(socket_path), arguments);
    }
    if (action == "output") {
        return parse_output(std::move(socket_path), arguments);
    }
    return parse_failure("unknown attempt action");
}

auto print_attempt_result(Command const& command, ControlReply const& value) -> bool
{
    if (command.kind == CommandKind::AttemptList) {
        auto const* page = std::get_if<AttemptPage>(&value);
        if (!page) {
            return false;
        }
        auto text = std::string{};
        for (auto const& attempt : page->items) {
            auto line = summary_line(attempt);
            if (!line) {
                return false;
            }
            text += *line;
        }
        if (page->items.empty()) {
            text = "No attempts\n";
        }
        if (page->next_cursor) {
            text += fmt::format("Next cursor: {}\n", escape_human(*page->next_cursor));
        }
        fmt::print(stdout, "{}", text);
        return true;
    }
    if (command.kind == CommandKind::AttemptOutput) {
        auto const* chunk = std::get_if<AttemptOutputChunk>(&value);
        if (!chunk) {
            return false;
        }
        auto text = output_text(*chunk);
        if (!text) {
            return false;
        }
        fmt::print(stdout, "{}", *text);
        return true;
    }
    auto const* attempt = std::get_if<AttemptDetails>(&value);
    if (!attempt) {
        return false;
    }
    auto text = summary_line(*attempt);
    if (!text) {
        return false;
    }
    if (attempt->result) {
        auto result = serialize_json(*attempt->result);
        if (!result) {
            return false;
        }
        *text += fmt::format("  Result: {}\n", escape_human(*result));
    }
    fmt::print(stdout, "{}", *text);
    return true;
}

} // namespace jb::jobuctl::detail
