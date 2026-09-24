#include "commands_priv.hpp"

#include "command_helpers_priv.hpp"
#include "control_json.hpp"
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

auto run_id_command(std::filesystem::path socket_path, CommandKind kind, std::span<CommandLineArgument const> arguments)
    -> CommandBuildResult
{
    if (arguments.size() != 1U || arguments.front().kind() != CommandLineArgumentKind::Positional) {
        return parse_failure("run command requires one run UUID");
    }
    auto const params =
        JsonValue{.data = JsonValue::Object{{"run_id", JsonValue{.data = std::string{arguments.front().token()}}}}};
    auto id = kind == CommandKind::RunGet ? run_get_request_from_json(params) : cancel_run_request_from_json(params);
    if (!id) {
        return parse_failure("run command requires a canonical run UUID");
    }
    return {
        .command = Command{.socket_path = std::move(socket_path), .kind = kind, .request = std::move(id).value()}
    };
}

auto parse_run_list(std::filesystem::path socket_path, std::span<CommandLineArgument const> arguments)
    -> CommandBuildResult
{
    auto params    = JsonValue::Object{};
    auto planned   = JsonValue::Object{};
    auto started   = JsonValue::Object{};
    auto completed = JsonValue::Object{};

    // Build the public wire shape, then let its strict codec enforce enum, UTC range, and cursor rules.
    for (auto const& argument : arguments) {
        auto value = option_value(argument);
        if (!value) {
            return parse_failure("run list requires nonempty option values");
        }
        auto const name = argument.name();
        if (name == "limit") {
            auto parsed = parse_unsigned(*value, 1, 200);
            if (!parsed) {
                return parse_failure("--limit must be from 1 through 200");
            }
            params.emplace("limit", JsonValue{.data = *parsed});
        }
        else if (name.ends_with("-from") || name.ends_with("-to")) {
            auto const prefix = name.substr(0, name.find('-'));
            auto*      range  = &planned;
            if (prefix == "started") {
                range = &started;
            }
            else if (prefix == "completed") {
                range = &completed;
            }
            range->emplace(name.ends_with("-from") ? "from" : "to", JsonValue{.data = std::string{*value}});
        }
        else {
            auto wire_name = std::string{name};
            if (name == "queue-id" || name == "job-id") {
                wire_name[wire_name.find('-')] = '_';
            }
            params.emplace(std::move(wire_name), JsonValue{.data = std::string{*value}});
        }
    }
    if (!planned.empty()) {
        params.emplace("planned", JsonValue{.data = std::move(planned)});
    }
    if (!started.empty()) {
        params.emplace("started", JsonValue{.data = std::move(started)});
    }
    if (!completed.empty()) {
        params.emplace("completed", JsonValue{.data = std::move(completed)});
    }

    auto request = run_list_request_from_json(JsonValue{.data = std::move(params)});
    if (!request) {
        return parse_failure("run list filters, ranges, or cursor are invalid");
    }
    return {
        .command = Command{.socket_path = std::move(socket_path),
                           .kind        = CommandKind::RunList,
                           .request     = std::move(request).value()}
    };
}

auto summary_line(RunSummary const& run) -> std::optional<std::string>
{
    auto encoded = run_summary_to_json(run);
    if (!encoded) {
        return std::nullopt;
    }
    auto const& fields    = encoded->as_object();
    auto        started   = serialize_json(fields.at("started_at"));
    auto        completed = serialize_json(fields.at("completed_at"));
    if (!started || !completed) {
        return std::nullopt;
    }
    return fmt::format("Run {}: state={}, origin={}, type={}, planned_at={}\n"
                       "  job_id={}, queue_id={}, revision={}, schedule_owned={}, priority={}, runnable_at={}, "
                       "started_at={}, completed_at={}\n",
                       run.id.to_string(),
                       fields.at("state").as_string(),
                       fields.at("origin").as_string(),
                       fields.at("type").as_string(),
                       fields.at("planned_at").as_string(),
                       run.job_id.to_string(),
                       run.queue_id.to_string(),
                       run.job_revision,
                       run.schedule_owned,
                       run.priority,
                       fields.at("runnable_at").as_string(),
                       *started,
                       *completed);
}

auto details_text(RunDetails const& run, StandardAttributeRegistry const& registry) -> std::optional<std::string>
{
    auto summary = summary_line(run);
    if (!summary) {
        return std::nullopt;
    }
    auto encoded = run_details_to_json(run, registry);
    if (!encoded) {
        return std::nullopt;
    }
    auto const& fields     = encoded->as_object();
    auto        attributes = serialize_json(fields.at("attributes"));
    auto        payload    = serialize_json(fields.at("payload"));
    auto        result     = serialize_json(fields.at("result"));
    if (!attributes || !payload || !result) {
        return std::nullopt;
    }
    *summary += fmt::format("  Attributes: {}\n  Payload: {}\n  Result: {}\n",
                            escape_human(*attributes),
                            escape_human(*payload),
                            escape_human(*result));
    return summary;
}

} // namespace

auto parse_run_command(std::filesystem::path                socket_path,
                       std::string_view                     action,
                       std::span<CommandLineArgument const> arguments,
                       StandardAttributeRegistry const&     registry) -> CommandBuildResult
{
    static_cast<void>(registry);
    if (action == "get") {
        return run_id_command(std::move(socket_path), CommandKind::RunGet, arguments);
    }
    if (action == "cancel") {
        return run_id_command(std::move(socket_path), CommandKind::RunCancel, arguments);
    }
    if (action == "list") {
        return parse_run_list(std::move(socket_path), arguments);
    }
    return parse_failure("unknown run action");
}

auto print_run_result(Command const& command, ControlReply const& value, StandardAttributeRegistry const& registry)
    -> bool
{
    if (command.kind == CommandKind::RunList) {
        auto const* page = std::get_if<RunPage>(&value);
        if (!page) {
            return false;
        }
        auto text = std::string{};
        for (auto const& run : page->items) {
            auto line = summary_line(run);
            if (!line) {
                return false;
            }
            text += *line;
        }
        if (page->items.empty()) {
            text = "No runs\n";
        }
        if (page->next_cursor) {
            text += fmt::format("Next cursor: {}\n", escape_human(*page->next_cursor));
        }
        fmt::print(stdout, "{}", text);
        return true;
    }
    if (command.kind == CommandKind::RunCancel) {
        auto const* cancelled = std::get_if<CancelRunResult>(&value);
        if (!cancelled) {
            return false;
        }
        auto line = summary_line(RunSummary{.id             = cancelled->run.id,
                                            .job_id         = cancelled->run.job_id,
                                            .queue_id       = cancelled->run.queue_id,
                                            .job_revision   = cancelled->run.job_revision,
                                            .origin         = cancelled->run.origin,
                                            .type           = cancelled->run.type,
                                            .state          = cancelled->run.state,
                                            .schedule_owned = cancelled->run.schedule_owned,
                                            .priority       = cancelled->run.priority,
                                            .planned_at     = cancelled->run.planned_at,
                                            .runnable_at    = cancelled->run.runnable_at,
                                            .started_at     = cancelled->run.started_at,
                                            .completed_at   = cancelled->run.completed_at});
        if (!line) {
            return false;
        }
        fmt::print(stdout,
                   "Cancellation {}: {}",
                   cancelled->disposition == CancelDisposition::Completed ? "completed" : "requested",
                   *line);
        return true;
    }
    auto const* run = std::get_if<RunDetails>(&value);
    if (!run) {
        return false;
    }
    auto text = details_text(*run, registry);
    if (!text) {
        return false;
    }
    fmt::print(stdout, "{}", *text);
    return true;
}

} // namespace jb::jobuctl::detail
