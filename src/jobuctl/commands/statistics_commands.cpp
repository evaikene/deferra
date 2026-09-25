#include "commands_priv.hpp"

#include "command_helpers_priv.hpp"
#include "json.hpp"
#include "statistics_json.hpp"

#include <fmt/format.h>

#include <cstdio> // IWYU pragma: keep for stdout macro
#include <string>
#include <utility>

namespace jb::jobuctl::detail {

using namespace jb::core;
using namespace jb::jobu;

namespace {

auto statistics_params(std::span<CommandLineArgument const> arguments, bool queue_scope) -> std::optional<JsonValue>
{
    auto fields  = JsonValue::Object{};
    auto planned = JsonValue::Object{};

    // Match the public request shape so its strict decoder owns filter, selector, and cursor rules.
    for (auto const& argument : arguments) {
        auto value = option_value(argument);
        if (!value) {
            return std::nullopt;
        }
        auto const name = argument.name();
        if (name == "limit") {
            auto limit = parse_unsigned(*value, 1, 200);
            if (!limit) {
                return std::nullopt;
            }
            fields.emplace("limit", JsonValue{.data = *limit});
        }
        else if (name == "planned-from" || name == "planned-to") {
            planned.emplace(name == "planned-from" ? "from" : "to", JsonValue{.data = std::string{*value}});
        }
        else {
            auto wire_name = std::string{name};
            if (queue_scope && name == "id") {
                wire_name = "queue_id";
            }
            else if (queue_scope && name == "name") {
                wire_name = "queue_name";
            }
            else if (name == "queue-id" || name == "job-id" || name == "group-by") {
                wire_name[wire_name.find('-')] = '_';
            }
            fields.emplace(std::move(wire_name), JsonValue{.data = std::string{*value}});
        }
    }
    if (!planned.empty()) {
        fields.emplace("planned", JsonValue{.data = std::move(planned)});
    }
    return JsonValue{.data = std::move(fields)};
}

auto duration_text(StatisticsDuration const& duration) -> std::string
{
    auto const average = duration.average ? fmt::format("{}", *duration.average) : "null";
    auto const maximum = duration.maximum ? fmt::format("{}", *duration.maximum) : "null";
    return fmt::format("samples={}, average={}, maximum={}", duration.samples, average, maximum);
}

} // namespace

auto parse_system_statistics_command(std::filesystem::path                socket_path,
                                     std::string_view                     action,
                                     std::span<CommandLineArgument const> arguments,
                                     StandardAttributeRegistry const& /*registry*/) -> CommandBuildResult
{
    if (action != "stats") {
        return parse_failure("unknown system command");
    }
    auto params = statistics_params(arguments, false);
    if (!params) {
        return parse_failure("system stats options are invalid");
    }
    auto request = system_statistics_request_from_json(*params);
    if (!request) {
        return parse_failure("system stats filters, window, or cursor are invalid");
    }
    return {
        .command = Command{.socket_path = std::move(socket_path),
                           .kind        = CommandKind::SystemStats,
                           .request     = std::move(request).value()}
    };
}

auto parse_queue_statistics_command(std::filesystem::path                socket_path,
                                    std::string_view                     action,
                                    std::span<CommandLineArgument const> arguments,
                                    StandardAttributeRegistry const& /*registry*/) -> CommandBuildResult
{
    if (action != "stats") {
        return parse_failure("unknown queue command");
    }
    auto params = statistics_params(arguments, true);
    if (!params) {
        return parse_failure("queue stats options are invalid");
    }
    auto request = queue_statistics_request_from_json(*params);
    if (!request) {
        return parse_failure("queue stats selector, filters, window, or cursor are invalid");
    }
    return {
        .command = Command{.socket_path = std::move(socket_path),
                           .kind        = CommandKind::QueueStats,
                           .request     = std::move(request).value()}
    };
}

auto print_statistics_result(ControlReply const& reply) -> bool
{
    auto const* page = std::get_if<StatisticsPage>(&reply);
    if (!page) {
        return false;
    }
    auto encoded = statistics_page_to_json(*page);
    if (!encoded) {
        return false;
    }
    auto const& fields = encoded->as_object();
    auto const& window = fields.at("window").as_object();
    auto const& groups = fields.at("groups").as_array();

    fmt::print(stdout, "Window: [{}, {})\n", window.at("from").as_string(), window.at("to").as_string());
    fmt::print(stdout, "Group by: {}\n", fields.at("group_by").as_string());
    fmt::print(stdout,
               "Measurement: timing={}, runnable_wait={}, capture={}\n",
               escape_human(page->measurement.timing),
               escape_human(page->measurement.runnable_wait),
               escape_human(page->measurement.capture));

    for (auto index = std::size_t{0}; index < page->groups.size(); ++index) {
        auto const& key      = groups[index].as_object().at("key");
        auto const  key_text = key.is_null() ? std::string{"null"} : escape_human(key.as_string());
        auto const& group    = page->groups[index];
        fmt::print(stdout, "Group {}: {}\n", index + 1, key_text);
        fmt::print(stdout,
                   "  Runs: total={}, scheduled={}, running={}, retry_wait={}, succeeded={}, failed={}, "
                   "interrupted={}, cancelled={}, cli={}, http={}, scheduled_origin={}, manual_origin={}\n",
                   group.runs.total,
                   group.runs.scheduled,
                   group.runs.running,
                   group.runs.retry_wait,
                   group.runs.succeeded,
                   group.runs.failed,
                   group.runs.interrupted,
                   group.runs.cancelled,
                   group.runs.cli,
                   group.runs.http,
                   group.runs.scheduled_origin,
                   group.runs.manual_origin);
        fmt::print(stdout,
                   "  Attempts: total={}, pending={}, running={}, completed={}, succeeded={}, failed={}, "
                   "interrupted={}, cancelled={}, retries={}\n",
                   group.attempts.total,
                   group.attempts.pending,
                   group.attempts.running,
                   group.attempts.completed,
                   group.attempts.succeeded,
                   group.attempts.failed,
                   group.attempts.interrupted,
                   group.attempts.cancelled,
                   group.attempts.retries);
        fmt::print(stdout,
                   "  Capture: truncated_attempts={}, lost_attempts={}\n",
                   group.capture.truncated_attempts,
                   group.capture.lost_attempts);
        fmt::print(stdout, "  Schedule lateness (ms): {}\n", duration_text(group.schedule_lateness_ms));
        fmt::print(stdout, "  Execution wall duration (ms): {}\n", duration_text(group.execution_wall_duration_ms));
        fmt::print(stdout, "  Runnable wait (ms): unavailable\n");
    }
    fmt::print(stdout, "Next cursor: {}\n", page->next_cursor ? escape_human(*page->next_cursor) : std::string{"null"});
    return true;
}

} // namespace jb::jobuctl::detail
